#pragma once
// Bounds shared by the compute binding state and the compute entry points. Keeping them in one
// place stops the fixed arrays in Pass and the validation in d3d12_compute.inl from disagreeing.
namespace x2w_compute {
constexpr unsigned max_root_parameters = 32;   // storage for parameters and the Dispatch loops
constexpr unsigned max_root_constants = 64;    // 32-bit values per root constants parameter
constexpr unsigned max_retained_uploads = 1024;
}  // namespace x2w_compute
