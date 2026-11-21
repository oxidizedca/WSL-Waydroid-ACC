// Query WSL's D3D12 device; do not create a command queue or submit work.
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <directx/dxcore.h>
#include <cstdio>
#include <vector>
int main() {
    IDXCoreAdapterFactory* f = nullptr;
    if (FAILED(DXCoreCreateAdapterFactory(IID_IDXCoreAdapterFactory, reinterpret_cast<void**>(&f)))) return 1;
    IDXCoreAdapterList* list = nullptr;
    if (FAILED(f->CreateAdapterList(1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, IID_IDXCoreAdapterList, reinterpret_cast<void**>(&list)))) return 2;
    unsigned tested = 0;
    for (unsigned i = 0; i < list->GetAdapterCount(); ++i) {
        IDXCoreAdapter* a = nullptr;
        if (FAILED(list->GetAdapter(i, IID_IDXCoreAdapter, reinterpret_cast<void**>(&a)))) continue;
        bool hw = false;
        size_t size = 0;
        if (SUCCEEDED(a->GetProperty(DXCoreAdapterProperty::IsHardware, &hw)) && hw
            && SUCCEEDED(a->GetPropertySize(DXCoreAdapterProperty::DriverDescription, &size)) && size) {
            std::vector<char> label(size + 1);
            a->GetProperty(DXCoreAdapterProperty::DriverDescription, size, label.data());
            ID3D12Device* dev = nullptr;
            if (SUCCEEDED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, reinterpret_cast<void**>(&dev)))) {
                for (auto level : {D3D12_COMMAND_QUEUE_PRIORITY_NORMAL, D3D12_COMMAND_QUEUE_PRIORITY_HIGH}) {
                    D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY q = {D3D12_COMMAND_LIST_TYPE_DIRECT, (UINT)level, 0};
                    HRESULT hr = dev->CheckFeatureSupport(D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, &q, sizeof(q));
                    printf("%s,direct,%u,0x%08x,%d\n", label.data(), q.Priority, (unsigned)hr, (int)q.PriorityForTypeIsSupported);
                    if (SUCCEEDED(hr)) ++tested;
                }
                dev->Release();
            }
        }
        a->Release();
    }
    list->Release(); f->Release();
    return tested ? 0 : 3;
}
