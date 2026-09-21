/* Vtable slot indices for the ID3D12Device resource-creation methods, computed from the SDK's
 * C interface definitions so they can never drift from the real ABI. */
#define COBJMACROS
#define CINTERFACE
#include <stddef.h>
#include <d3d12.h>

#define SLOT(iface, method) (offsetof(iface##Vtbl, method) / sizeof(void*))

const size_t vr_slot_CreateCommittedResource = SLOT(ID3D12Device, CreateCommittedResource);
const size_t vr_slot_CreatePlacedResource = SLOT(ID3D12Device, CreatePlacedResource);
const size_t vr_slot_CreateCommittedResource1 = SLOT(ID3D12Device4, CreateCommittedResource1);
const size_t vr_slot_CreateCommittedResource2 = SLOT(ID3D12Device8, CreateCommittedResource2);
const size_t vr_slot_CreatePlacedResource1 = SLOT(ID3D12Device8, CreatePlacedResource1);
const size_t vr_slot_CreateCommittedResource3 = SLOT(ID3D12Device10, CreateCommittedResource3);
const size_t vr_slot_CreatePlacedResource2 = SLOT(ID3D12Device10, CreatePlacedResource2);
