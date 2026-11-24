"""Source-built compute dispatch fixture for the startup runtime (#1412).

Builds a desktop compute container with DXC, wraps it as a synthetic Scarlett container, publishes
the runtime request, retargets and signs it offline, then dispatches it through the runtime wrapper
and compares all 64 readback words. Also exercises the refusal, tamper, missing-argument, repeated
dispatch and pipeline-classification controls.
"""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

TESTS = Path(__file__).resolve().parent
ROOT = TESTS.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(TESTS))

import dxil_retarget as rt
from dxil_retarget_check import _inject_xb2_into_container

SKIP = 77


def run(args, env, *, check=True):
    result = subprocess.run(args, env=env, capture_output=True, text=True)
    if check and result.returncode:
        raise RuntimeError(f"{' '.join(map(str, args))} -> {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result


def compile_shader(dxc, source, output):
    subprocess.run([str(dxc), "-T", "cs_6_0", "-E", "main", "-Fo", str(output), str(source)],
                   check=True, capture_output=True, text=True)
    return output.read_bytes()


def wrap_and_sign(container, root_probe):
    wrapped = _inject_xb2_into_container(container)
    retargeted, _ = rt.retarget_container(wrapped)
    return rt.sign_container(Path(root_probe), retargeted)


def expected_words(sentinel=0xDEADBEEF, dispatches=1):
    constants = [0] * 16
    constants[1] = 3
    constants[12] = 1
    constants[13] = 7
    constants[14] = 11
    constants[15] = 13
    key = constants[13] + 3 * constants[14] + 5 * constants[15]
    words = []
    for i in range(64):
        source = i * 13 + 7
        value = ((source * 33) ^ (key + i * constants[1])) & 0xFFFFFFFF
        words.append((sentinel + dispatches * (value + constants[12])) & 0xFFFFFFFF)
    return words


def check(consumer, dxc, root_probe, device_probe, backend):
    env = dict(os.environ, X2W_D3D12_PERMISSIVE="1", X2W_D3D12_RENDER="1", X2W_D3D12_LIST_NOOP="1")
    env.setdefault("X2W_D3D12_DEBUG", "0")
    debug_layer = env.get("X2W_D3D12_DEBUG") == "1"
    debug_layer_line = ""
    debug_messages: list[str] = []

    def note_debug(result):
        nonlocal debug_layer_line
        if not debug_layer:
            return
        for line in result.stderr.splitlines():
            if "x2w-d3d12 debug layer:" in line:
                debug_layer_line = line.strip()
            elif "x2w-d3d12-debug" in line:
                debug_messages.append(line.strip())

    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        # Cross-language golden vectors: the C++ export and the Python request writer must agree.
        for index, data in enumerate((b"", b"abc", b"DXBC" + bytes(range(64)))):
            path = directory / f"key{index}.bin"
            path.write_bytes(data)
            result = run([str(consumer), backend, "key", str(path)], env)
            printed = int(result.stdout.strip().split("=", 1)[1], 16)
            python = rt.compute_cache_key(data)
            assert printed == python, (data, hex(printed), hex(python))
        print("cross-language key vectors: empty, abc and 68-byte DXBC agree")

        desktop = compile_shader(dxc, TESTS / "startup_compute.hlsl", directory / "desktop.cso")
        wrapped = _inject_xb2_into_container(desktop)
        shader = directory / "shader.xb2"
        shader.write_bytes(wrapped)
        cache = directory / "cache"
        cache.mkdir()
        env["X2W_SHADER_CACHE"] = str(cache)
        output = directory / "output.bin"

        prepared = run([str(consumer), backend, "prepare", str(shader), str(output)], env, check=False)
        if prepared.returncode == SKIP:
            print(f"{backend}: no adapter (capability skip)")
            return SKIP
        if prepared.returncode:
            raise RuntimeError(f"prepare failed: {prepared.returncode}\n{prepared.stdout}\n{prepared.stderr}")
        note_debug(prepared)
        assert "pipeline=placeholder" in prepared.stdout, prepared.stdout
        requests = sorted(cache.glob("req_*.cs"))
        assert len(requests) == 1, requests
        key = requests[0].name[4:-3]
        assert len(key) == 16 and int(key, 16) == rt.compute_cache_key(wrapped)
        request_manifest = rt._read_manifest(cache / f"{key}.cs.src")
        assert request_manifest["source_sha256"] == hashlib.sha256(wrapped).hexdigest()
        print(f"request {key}: source SHA-256 {request_manifest['source_sha256']}, placeholder before signing")

        pending_args = [sys.executable, "-B", str(Path(rt.__file__)), "--pending", str(cache),
                        "--probe", str(root_probe), "--pso-probe", str(device_probe)]
        if backend == "warp":
            pending_args.append("--warp-only")
        pending = run(pending_args, env, check=False)
        if pending.returncode:
            combined = pending.stdout + pending.stderr
            if backend == "warp" and "warp_pso" in combined:
                print(f"{backend}: compute PSO unavailable in the PSO probe (capability skip)\n{combined}")
                return SKIP
            raise RuntimeError(f"pending failed: {pending.returncode}\n{combined}")
        assert "signed" in pending.stdout, pending.stdout
        manifest = rt._read_manifest(cache / f"{key}.cs.manifest")
        signed = (cache / f"{key}.cs.cso").read_bytes()
        assert manifest["status"] == "signed", manifest
        assert manifest.get("pso_probe") == "PASSED", manifest
        assert manifest["source_sha256"] == hashlib.sha256(wrapped).hexdigest(), manifest
        assert manifest["output_sha256"] == hashlib.sha256(signed).hexdigest(), manifest
        assert signed[:4] == b"DXBC" and signed[4:20] != bytes(16), signed[:20].hex()
        print(f"signed cache: {len(signed)} bytes, output SHA-256 {manifest['output_sha256']}")

        table = run([str(consumer), backend, "prepare-table", str(shader), str(output)], env, check=False)
        if table.returncode not in (0, SKIP):
            raise RuntimeError(f"prepare-table failed: {table.returncode}\n{table.stdout}\n{table.stderr}")
        if table.returncode == 0:
            note_debug(table)
            assert "pipeline=placeholder" in table.stdout, table.stdout
            assert "descriptor table parameters are not supported" in table.stderr, table.stderr
            print(f"{backend} descriptor-table root: pipeline refused with a reason")

        unwrapped = run([str(consumer), backend, "prepare-unwrapped", str(shader), str(output)], env, check=False)
        if unwrapped.returncode not in (0, SKIP):
            raise RuntimeError(f"prepare-unwrapped failed: {unwrapped.returncode}\n{unwrapped.stdout}\n"
                               f"{unwrapped.stderr}")
        if unwrapped.returncode == 0:
            note_debug(unwrapped)
            assert "pipeline=placeholder" in unwrapped.stdout, unwrapped.stdout
            assert "not a wrapped desktop root" in unwrapped.stderr, unwrapped.stderr
            print(f"{backend} unwrapped desktop root: pipeline refused, no fallback root")

        def execute(variant, extra_env=None):
            local_env = dict(env, **(extra_env or {}))
            result = run([str(consumer), backend, "execute", variant, str(shader), str(output)], local_env,
                         check=False)
            if result.returncode == SKIP:
                return SKIP, result
            if result.returncode:
                raise RuntimeError(f"{variant} failed: {result.returncode}\n{result.stdout}\n{result.stderr}")
            note_debug(result)
            return 0, result

        def read_words(path):
            data = path.read_bytes()
            assert len(data) == 64 * 4, len(data)
            return list(struct.unpack("<64I", data))

        def dispatch_line(result):
            return next(line for line in result.stdout.splitlines() if line.startswith("dispatch_count="))

        rc, normal = execute("normal")
        if rc == SKIP:
            print(f"{backend}: existing heaps unavailable (capability skip)")
            return SKIP
        assert "pipeline=real" in normal.stdout, normal.stdout
        before = read_words(directory / "output.bin.before")
        after = read_words(output)
        assert before == [0xDEADBEEF] * 64, before
        assert after == expected_words(), (after[:4], expected_words()[:4])
        assert "dispatch_count=1 skipped=0" in normal.stdout, normal.stdout
        print(f"{backend} normal: before={before[:4]} after={after[:4]} {dispatch_line(normal)}")
        print("normal all 64 before:", " ".join(f"{word:08x}" for word in before))
        print("normal all 64 after: ", " ".join(f"{word:08x}" for word in after))

        rc, partial = execute("partial")
        assert rc == 0 and read_words(output) == expected_words(), partial.stdout
        print(f"{backend} partial root constants: 4+12 split, 64 words exact, {dispatch_line(partial)}")

        rc, repeated = execute("repeat")
        assert rc == 0 and read_words(output) == expected_words(dispatches=2), repeated.stdout
        assert "dispatch_count=2" in repeated.stdout, repeated.stdout
        print(f"{backend} repeated dependent Dispatch: 2 UAV barriers, 64 words exact, {dispatch_line(repeated)}")

        controls = {
            "missing-root": (0, "no real compute root signature"),
            "bad-uav": (0, "not DWORD-aligned"),
            "same-binding": (0, "both root SRV and root UAV"),
            "large-count": (0, "root constant range exceeds the declared count"),
            "graphics-dispatch": (0, "compute pipeline"),
        }
        for variant, (dispatches, message) in controls.items():
            rc, result = execute(variant)
            assert rc == 0, result.stdout
            assert read_words(output) == [0xDEADBEEF] * 64, (variant, read_words(output)[:4])
            assert f"dispatch_count={dispatches}" in result.stdout, (variant, result.stdout)
            combined = result.stdout + result.stderr
            assert message in combined, (variant, combined[-2000:])
            print(f"{backend} {variant}: readback unchanged; {message}; {dispatch_line(result)}")

        rc, wide = execute("wide-root")
        assert rc == 0 and read_words(output) == expected_words(), wide.stdout
        combined = wide.stdout + wide.stderr
        assert "more than 32 root parameters" in combined, combined[-2000:]
        assert "dispatch_count=1 skipped=2" in wide.stdout, wide.stdout
        print(f"{backend} wide-root (34 parameters): refused before and after a graphics round trip, then "
              f"recovered; {dispatch_line(wide)}")

        rc, offsets = execute("offset-constants")
        assert rc == 0 and read_words(output) == expected_words(), offsets.stdout
        assert "dispatch_count=1 skipped=0" in offsets.stdout, offsets.stdout
        print(f"{backend} nonzero-offset constants: reverse 4x4 writes, 64 words exact, {dispatch_line(offsets)}")

        rc, incomplete = execute("incomplete")
        assert rc == 0 and read_words(output) == [0xDEADBEEF] * 64, incomplete.stdout
        combined = incomplete.stdout + incomplete.stderr
        assert "not fully initialized" in combined, combined[-2000:]
        assert "dispatch_count=0 skipped=1" in incomplete.stdout, incomplete.stdout
        print(f"{backend} incomplete constants: 12 of 16 DWORDs refused, sentinel unchanged, "
              f"{dispatch_line(incomplete)}")

        for variant, message in (("bad-offset", "root constant offset exceeds the declared count"),
                                 ("bad-count", "root constant range exceeds the declared count")):
            rc, result = execute(variant)
            assert rc == 0 and read_words(output) == [0xDEADBEEF] * 64, result.stdout
            combined = result.stdout + result.stderr
            assert message in combined, (variant, combined[-2000:])
            assert "dispatch_count=0 skipped=1" in result.stdout, result.stdout
            print(f"{backend} {variant}: {message}; sentinel unchanged; {dispatch_line(result)}")

        for variant, label in (("reset-constants", "Reset"),
                               ("root-invalidate", "a different root signature")):
            rc, result = execute(variant)
            assert rc == 0 and read_words(output) == expected_words(), result.stdout + result.stderr
            combined = result.stdout + result.stderr
            assert "not fully initialized" in combined, (variant, combined[-2000:])
            assert "dispatch_count=1 skipped=1" in result.stdout, result.stdout
            print(f"{backend} {variant}: {label} dropped the initialized-DWORD mask; first output exact, "
                  f"{dispatch_line(result)}")

        rc, index31 = execute("root-index-31")
        assert rc == 0 and read_words(output) == [0xDEADBEEF] * 64, index31.stdout
        combined = index31.stdout + index31.stderr
        assert "does not match the PSO" in combined, combined[-2000:]
        assert "invalid SetComputeRoot32BitConstant" not in combined, combined[-2000:]
        print(f"{backend} root parameter 31: accepted, Dispatch refused for the root mismatch; "
              f"{dispatch_line(index31)}")

        rc, index32 = execute("root-index-32")
        assert rc == 0 and read_words(output) == [0xDEADBEEF] * 64, index32.stdout
        combined = index32.stdout + index32.stderr
        assert "invalid SetComputeRoot32BitConstant arguments" in combined, combined[-2000:]
        print(f"{backend} root parameter 32: refused as out of range; {dispatch_line(index32)}")

        rc, uploads = execute("upload-limit", {"X2W_TEST_COMPUTE_UPLOAD_LIMIT": "2"})
        assert rc == 0 and read_words(output) == expected_words(dispatches=2), uploads.stdout
        combined = uploads.stdout + uploads.stderr
        assert "retained compute upload limit (2) is reached" in combined, combined[-2000:]
        assert "dispatch_count=2 skipped=1" in uploads.stdout, uploads.stdout
        print(f"{backend} upload limit: two uploads retained, third refused, 64 words for two dispatches; "
              f"{dispatch_line(uploads)}")

        rc, computed_draw = execute("compute-draw")
        assert rc == 0 and read_words(output) == expected_words(), computed_draw.stdout
        combined = computed_draw.stdout + computed_draw.stderr
        assert "skipped: a compute pipeline is bound" in combined, combined[-2000:]
        print(f"{backend} compute-draw: draw skipped, compute output exact")

        rc, stale = execute("reset-stale")
        assert rc == 0 and read_words(output) == expected_words(), stale.stdout
        assert "dispatch_count=1" in stale.stdout, stale.stdout
        print(f"{backend} reset-stale: second Dispatch skipped, first output exact, {dispatch_line(stale)}")

        # Missing cache: the same execute path must leave the sentinel untouched.
        empty_cache = directory / "empty"
        empty_cache.mkdir()
        env["X2W_SHADER_CACHE"] = str(empty_cache)
        rc, missing = execute("normal")
        assert rc == 0 and read_words(output) == [0xDEADBEEF] * 64, missing.stdout
        assert "pipeline=placeholder" in missing.stdout and "dispatch_count=0" in missing.stdout, missing.stdout
        print(f"{backend} missing cache: placeholder, sentinel unchanged, {dispatch_line(missing)}")

        # Explicit refusal manifest.
        env["X2W_SHADER_CACHE"] = str(cache)
        (cache / f"{key}.cs.cso").unlink(missing_ok=True)
        (cache / f"{key}.cs.manifest").write_text(
            f"x2w-compute-cache 1\nkey {key}\nrevision {rt.COMPUTE_CACHE_REVISION}\nstatus refused\n"
            f"source_bytes {len(wrapped)}\nsource_sha256 {hashlib.sha256(wrapped).hexdigest()}\n"
            f"reason REJECT: test refusal\n", encoding="utf-8")
        rc, refused = execute("normal")
        assert rc == 0 and read_words(output) == [0xDEADBEEF] * 64, refused.stdout
        assert "pipeline=placeholder" in refused.stdout and "REJECT: test refusal" in refused.stderr, refused.stderr
        print(f"{backend} explicit refusal: placeholder, reason logged, sentinel unchanged, {dispatch_line(refused)}")

        # Restore a signed entry, then tamper with it under the same manifest.
        (cache / f"{key}.cs.cso").write_bytes(signed)
        (cache / f"{key}.cs.manifest").write_text(
            f"x2w-compute-cache 1\nkey {key}\nrevision {rt.COMPUTE_CACHE_REVISION}\nstatus signed\n"
            f"source_bytes {len(wrapped)}\nsource_sha256 {hashlib.sha256(wrapped).hexdigest()}\n"
            f"output_bytes {len(signed)}\noutput_sha256 {hashlib.sha256(signed).hexdigest()}\n",
            encoding="utf-8")
        tampered = bytearray(signed)
        tampered[-1] ^= 0x01
        (cache / f"{key}.cs.cso").write_bytes(bytes(tampered))
        rc, tamper = execute("normal")
        assert rc == 0 and read_words(output) == [0xDEADBEEF] * 64, tamper.stdout
        assert "hash or size mismatch" in tamper.stderr, tamper.stderr
        print(f"{backend} tampered signed output: placeholder, hash mismatch logged, sentinel unchanged, "
              f"{dispatch_line(tamper)}")

        # A different, validly signed shader under the same key is still rejected by the output hash.
        other_source = directory / "other.hlsl"
        other_source.write_text((TESTS / "startup_compute.hlsl").read_text(encoding="utf-8")
                                .replace("previous + value + config[3].x", "previous + value + 2u + config[3].x"),
                                encoding="utf-8")
        other_desktop = compile_shader(dxc, other_source, directory / "other.cso")
        other_signed = wrap_and_sign(other_desktop, root_probe)
        (cache / f"{key}.cs.cso").write_bytes(other_signed)
        rc, wrong = execute("normal")
        assert rc == 0 and read_words(output) == [0xDEADBEEF] * 64, wrong.stdout
        assert "hash or size mismatch" in wrong.stderr, wrong.stderr
        print(f"{backend} wrong shader under the correct key: placeholder, sentinel unchanged, {dispatch_line(wrong)}")

        # A signed manifest that reports a failed PSO probe is contradictory and must not be used.
        (cache / f"{key}.cs.cso").write_bytes(signed)
        (cache / f"{key}.cs.manifest").write_text(
            f"x2w-compute-cache 1\nkey {key}\nrevision {rt.COMPUTE_CACHE_REVISION}\nstatus signed\n"
            f"pso_probe FAILED test\n"
            f"source_bytes {len(wrapped)}\nsource_sha256 {hashlib.sha256(wrapped).hexdigest()}\n"
            f"output_bytes {len(signed)}\noutput_sha256 {hashlib.sha256(signed).hexdigest()}\n",
            encoding="utf-8")
        rc, failed_probe = execute("normal")
        assert rc == 0 and read_words(output) == [0xDEADBEEF] * 64, failed_probe.stdout
        assert "reports a failed PSO probe" in failed_probe.stderr, failed_probe.stderr
        print(f"{backend} failed PSO-probe marker: placeholder, sentinel unchanged, "
              f"{dispatch_line(failed_probe)}")

        if debug_layer:
            assert "0x00000000 enabled" in debug_layer_line, debug_layer_line or "no debug-layer line"
            print(f"{backend} raw: {debug_layer_line}")
            serious = [message for message in debug_messages if "sev=0" in message or "sev=1" in message]
            warnings = [message for message in debug_messages if "sev=2" in message]
            print(f"{backend} debug layer: enabled; {len(debug_messages)} messages, "
                  f"{len(serious)} error/corruption, {len(warnings)} warning")
            for message in (serious + warnings)[:10]:
                print(f"debug: {message}")
            assert not serious, serious[:10]

        return 0


if __name__ == "__main__":
    if len(sys.argv) != 6:
        raise SystemExit("usage: startup_compute_check.py <consumer> <dxc> <root_probe> <root_device_probe> <warp|hardware>")
    sys.exit(check(*map(Path, sys.argv[1:5]), sys.argv[5]))
