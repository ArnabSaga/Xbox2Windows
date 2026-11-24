// Source-built compute fixture for the startup runtime's Dispatch path (#1412).
// Root signature: SRV(t0), UAV(u0), 16 root constants in b0.
// The shader reads its previous UAV value, so a second Dispatch must be ordered by a UAV barrier.
#define X2W_ROOT_SIGNATURE "SRV(t0),UAV(u0),RootConstants(num32BitConstants=16,b0)"

ByteAddressBuffer sourceData : register(t0);
RWByteAddressBuffer resultData : register(u0);
cbuffer Config : register(b0) { uint4 config[4]; }

[RootSignature(X2W_ROOT_SIGNATURE)]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint i = id.x;
    uint value = (sourceData.Load(4u * i) * 33u) ^
                 (config[3].y + 3u * config[3].z + 5u * config[3].w + i * config[0].y);
    uint previous = resultData.Load(4u * i);
    resultData.Store(4u * i, previous + value + config[3].x);
}
