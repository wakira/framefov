// Minimal D3D11 smoke test for reproducing "Vulkan layer + DXVK" interactions.
//
#include <cstdlib>
// Creates a D3D11 device through DXVK, renders a triangle into an offscreen
// render target (which makes DXVK build Vulkan render passes / framebuffers and
// issue draws), then releases everything. Prints progress and exits non-zero on
// failure, so a crash or a device-creation failure is obvious.
#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <cstdio>
#include <cstring>

#define CHECK_HR(expr)                                                          \
  do {                                                                          \
    HRESULT hr_ = (expr);                                                       \
    if (FAILED(hr_)) {                                                          \
      printf("FAIL %s -> 0x%08lx\n", #expr, (unsigned long)hr_);                \
      return 1;                                                                 \
    }                                                                           \
  } while (0)

static const char* kShader = R"(
struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };
VSOut vs_main(uint id : SV_VertexID) {
  VSOut o;
  float2 p[3] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };
  o.pos = float4(p[id], 0, 1);
  o.col = float4(0.2 + 0.3 * id, 0.5, 0.9 - 0.2 * id, 1);
  return o;
}
float4 ps_main(VSOut i) : SV_TARGET { return i.col; }
)";

int main(int argc, char** argv) {
  const UINT width = argc > 1 ? (UINT)atoi(argv[1]) : 64;
  const UINT height = argc > 2 ? (UINT)atoi(argv[2]) : 64;
  const int draws = argc > 3 ? atoi(argv[3]) : 3;
  printf("render target %ux%u, %d draws per frame\n", width, height, draws);
  fflush(stdout);
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  UINT flags = 0;
  printf("D3D11CreateDevice ...\n");
  fflush(stdout);
  CHECK_HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                             D3D11_SDK_VERSION, &device, &level, &context));
  printf("device created (feature level 0x%x)\n", (unsigned)level);
  fflush(stdout);

  IDXGIDevice* dxgiDevice = nullptr;
  CHECK_HR(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice));
  IDXGIAdapter* adapter = nullptr;
  CHECK_HR(dxgiDevice->GetAdapter(&adapter));
  DXGI_ADAPTER_DESC desc{};
  adapter->GetDesc(&desc);
  printf("adapter: %ls\n", desc.Description);
  adapter->Release();
  dxgiDevice->Release();
  fflush(stdout);

  // Offscreen render target.
  D3D11_TEXTURE2D_DESC td{};
  td.Width = width;
  td.Height = height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* target = nullptr;
  CHECK_HR(device->CreateTexture2D(&td, nullptr, &target));
  ID3D11RenderTargetView* rtv = nullptr;
  CHECK_HR(device->CreateRenderTargetView(target, nullptr, &rtv));

  ID3DBlob* vsBlob = nullptr;
  ID3DBlob* psBlob = nullptr;
  ID3DBlob* error = nullptr;
  if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "vs_main", "vs_4_0",
                        0, 0, &vsBlob, &error))) {
    printf("FAIL: vertex shader compilation (%s)\n", error ? (const char*)error->GetBufferPointer() : "?");
    return 1;
  }
  if (FAILED(D3DCompile(kShader, strlen(kShader), nullptr, nullptr, nullptr, "ps_main", "ps_4_0",
                        0, 0, &psBlob, &error))) {
    printf("FAIL: pixel shader compilation (%s)\n", error ? (const char*)error->GetBufferPointer() : "?");
    return 1;
  }
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  CHECK_HR(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs));
  CHECK_HR(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps));
  printf("shaders created\n");
  fflush(stdout);

  const float clear[4] = {0.1f, 0.1f, 0.1f, 1.0f};
  for (int frame = 0; frame < 8; ++frame) {
    context->ClearRenderTargetView(rtv, clear);
    context->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0.f, 0.f, (float)width, (float)height, 0.f, 1.f};
    context->RSSetViewports(1, &vp);
    context->VSSetShader(vs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (int d = 0; d < draws; ++d) context->Draw(3, 0);
    context->Flush();
  }
  printf("8 frames drawn\n");
  fflush(stdout);

  context->ClearState();
  ps->Release();
  vs->Release();
  psBlob->Release();
  vsBlob->Release();
  rtv->Release();
  target->Release();
  context->Release();
  device->Release();
  printf("OK\n");
  return 0;
}
