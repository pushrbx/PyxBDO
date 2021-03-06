// ImGui Win32 + DirectX11 binding
// In this binding, ImTextureID is used to store a 'ID3D11ShaderResourceView*' texture identifier. Read the FAQ about ImTextureID in imgui.cpp.

// You can copy and use unmodified imgui_impl_* files in your project. See main.cpp for an example of using this.
// If you use this binding you'll need to call 4 functions: ImGui_ImplXXXX_Init(), ImGui_ImplXXXX_NewFrame(), ImGui::Render() and ImGui_ImplXXXX_Shutdown().
// If you are new to ImGui, see examples/README.txt and documentation at the top of imgui.cpp.
// https://github.com/ocornut/imgui

#include "imgui.h"
#include "imgui_impl_dx11.h"

// DirectX
#include <d3d11.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

// Data
static INT64                    g_Time = 0;
static INT64                    g_TicksPerSecond = 0;

static HWND                     g_hWnd = 0;
static ID3D11Device*            g_pd3dDevice = NULL;
static ID3D11DeviceContext*     g_pd3dDeviceContext = NULL;
static ID3D11Buffer*            g_pVB = NULL;
static ID3D11Buffer*            g_pIB = NULL;
static ID3D11VertexShader*      g_pVertexShader = NULL;
static ID3D11InputLayout*       g_pInputLayout = NULL;
static ID3D11Buffer*            g_pVertexConstantBuffer = NULL;
static ID3D11PixelShader*       g_pPixelShader = NULL;
static ID3D11SamplerState*      g_pFontSampler = NULL;
static ID3D11ShaderResourceView*g_pFontTextureView = NULL;
static ID3D11RasterizerState*   g_pRasterizerState = NULL;
static ID3D11BlendState*        g_pBlendState = NULL;
static int                      g_VertexBufferSize = 5000, g_IndexBufferSize = 10000;

struct VERTEX_CONSTANT_BUFFER
{
    float        mvp[4][4];
};

// This is the main rendering function that you have to implement and provide to ImGui (via setting up 'RenderDrawListsFn' in the ImGuiIO structure)
// If text or lines are blurry when integrating ImGui in your engine:
// - in your Render function, try translating your projection matrix by (0.5f,0.5f) or (0.375f,0.375f)
void ImGui_ImplDX11_RenderDrawLists(ImDrawData* draw_data)
{
    ID3D11DeviceContext* ctx = g_pd3dDeviceContext;

    // Create and grow vertex/index buffers if needed
    if (!g_pVB || g_VertexBufferSize < draw_data->TotalVtxCount)
    {
        if (g_pVB) { g_pVB->Release(); g_pVB = NULL; }
        g_VertexBufferSize = draw_data->TotalVtxCount + 5000;
        D3D11_BUFFER_DESC desc;
        memset(&desc, 0, sizeof(D3D11_BUFFER_DESC));
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.ByteWidth = g_VertexBufferSize * sizeof(ImDrawVert);
        desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = 0;
        if (g_pd3dDevice->CreateBuffer(&desc, NULL, &g_pVB) < 0)
            return;
    }
    if (!g_pIB || g_IndexBufferSize < draw_data->TotalIdxCount)
    {
        if (g_pIB) { g_pIB->Release(); g_pIB = NULL; }
        g_IndexBufferSize = draw_data->TotalIdxCount + 10000;
        D3D11_BUFFER_DESC desc;
        memset(&desc, 0, sizeof(D3D11_BUFFER_DESC));
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.ByteWidth = g_IndexBufferSize * sizeof(ImDrawIdx);
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (g_pd3dDevice->CreateBuffer(&desc, NULL, &g_pIB) < 0)
            return;
    }

    // Copy and convert all vertices into a single contiguous buffer
    D3D11_MAPPED_SUBRESOURCE vtx_resource, idx_resource;
    if (ctx->Map(g_pVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &vtx_resource) != S_OK)
        return;
    if (ctx->Map(g_pIB, 0, D3D11_MAP_WRITE_DISCARD, 0, &idx_resource) != S_OK)
        return;
    ImDrawVert* vtx_dst = (ImDrawVert*)vtx_resource.pData;
    ImDrawIdx* idx_dst = (ImDrawIdx*)idx_resource.pData;
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        memcpy(vtx_dst, &cmd_list->VtxBuffer[0], cmd_list->VtxBuffer.size() * sizeof(ImDrawVert));
        memcpy(idx_dst, &cmd_list->IdxBuffer[0], cmd_list->IdxBuffer.size() * sizeof(ImDrawIdx));
        vtx_dst += cmd_list->VtxBuffer.size();
        idx_dst += cmd_list->IdxBuffer.size();
    }
    ctx->Unmap(g_pVB, 0);
    ctx->Unmap(g_pIB, 0);

    // Setup orthographic projection matrix into our constant buffer
    {
        D3D11_MAPPED_SUBRESOURCE mapped_resource;
        if (ctx->Map(g_pVertexConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_resource) != S_OK)
            return;
        VERTEX_CONSTANT_BUFFER* constant_buffer = (VERTEX_CONSTANT_BUFFER*)mapped_resource.pData;
        float L = 0.0f;
        float R = ImGui::GetIO().DisplaySize.x;
        float B = ImGui::GetIO().DisplaySize.y;
        float T = 0.0f;
        float mvp[4][4] =
        {
            { 2.0f/(R-L),   0.0f,           0.0f,       0.0f },
            { 0.0f,         2.0f/(T-B),     0.0f,       0.0f },
            { 0.0f,         0.0f,           0.5f,       0.0f },
            { (R+L)/(L-R),  (T+B)/(B-T),    0.5f,       1.0f },
        };
        memcpy(&constant_buffer->mvp, mvp, sizeof(mvp));
        ctx->Unmap(g_pVertexConstantBuffer, 0);
    }

    // Backup DX state that will be modified to restore it afterwards (unfortunately this is very ugly looking and verbose. Close your eyes!)
    struct BACKUP_DX11_STATE
    {
        UINT                        ScissorRectsCount, ViewportsCount;
        D3D11_RECT                  ScissorRects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        D3D11_VIEWPORT              Viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        ID3D11RasterizerState*      RS;
        ID3D11BlendState*           BlendState;
        FLOAT                       BlendFactor[4];
        UINT                        SampleMask;
        ID3D11ShaderResourceView*   PSShaderResource;
        ID3D11SamplerState*         PSSampler;
        ID3D11PixelShader*          PS;
        ID3D11VertexShader*         VS;
        UINT                        PSInstancesCount, VSInstancesCount;
        ID3D11ClassInstance*        PSInstances[256], *VSInstances[256];   // 256 is max according to PSSetShader documentation
        D3D11_PRIMITIVE_TOPOLOGY    PrimitiveTopology;
        ID3D11Buffer*               IndexBuffer, *VertexBuffer, *VSConstantBuffer;
        UINT                        IndexBufferOffset, VertexBufferStride, VertexBufferOffset;
        DXGI_FORMAT                 IndexBufferFormat;
        ID3D11InputLayout*          InputLayout;
    };
    BACKUP_DX11_STATE old;
    old.ScissorRectsCount = old.ViewportsCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&old.ScissorRectsCount, old.ScissorRects);
    ctx->RSGetViewports(&old.ViewportsCount, old.Viewports);
    ctx->RSGetState(&old.RS);
    ctx->OMGetBlendState(&old.BlendState, old.BlendFactor, &old.SampleMask);
    ctx->PSGetShaderResources(0, 1, &old.PSShaderResource);
    ctx->PSGetSamplers(0, 1, &old.PSSampler);
    old.PSInstancesCount = old.VSInstancesCount = 256;
    ctx->PSGetShader(&old.PS, old.PSInstances, &old.PSInstancesCount);
    ctx->VSGetShader(&old.VS, old.VSInstances, &old.VSInstancesCount);
    ctx->VSGetConstantBuffers(0, 1, &old.VSConstantBuffer);
    ctx->IAGetPrimitiveTopology(&old.PrimitiveTopology);
    ctx->IAGetIndexBuffer(&old.IndexBuffer, &old.IndexBufferFormat, &old.IndexBufferOffset);
    ctx->IAGetVertexBuffers(0, 1, &old.VertexBuffer, &old.VertexBufferStride, &old.VertexBufferOffset);
    ctx->IAGetInputLayout(&old.InputLayout);

    // Setup viewport
    D3D11_VIEWPORT vp;
    memset(&vp, 0, sizeof(D3D11_VIEWPORT));
    vp.Width = ImGui::GetIO().DisplaySize.x;
    vp.Height = ImGui::GetIO().DisplaySize.y;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = vp.TopLeftY = 0.0f;
    ctx->RSSetViewports(1, &vp);

    // Bind shader and vertex buffers
    unsigned int stride = sizeof(ImDrawVert);
    unsigned int offset = 0;
    ctx->IASetInputLayout(g_pInputLayout);
    ctx->IASetVertexBuffers(0, 1, &g_pVB, &stride, &offset);
    ctx->IASetIndexBuffer(g_pIB, sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_pVertexShader, NULL, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_pVertexConstantBuffer);
    ctx->PSSetShader(g_pPixelShader, NULL, 0);
    ctx->PSSetSamplers(0, 1, &g_pFontSampler);

    // Setup render state
    const float blend_factor[4] = { 0.f, 0.f, 0.f, 0.f };
    ctx->OMSetBlendState(g_pBlendState, blend_factor, 0xffffffff);
    ctx->RSSetState(g_pRasterizerState);

    // Render command lists
    int vtx_offset = 0;
    int idx_offset = 0;
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];
        for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.size(); cmd_i++)
        {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback)
            {
                pcmd->UserCallback(cmd_list, pcmd);
            }
            else
            {
                const D3D11_RECT r = { (LONG)pcmd->ClipRect.x, (LONG)pcmd->ClipRect.y, (LONG)pcmd->ClipRect.z, (LONG)pcmd->ClipRect.w };
                ctx->PSSetShaderResources(0, 1, (ID3D11ShaderResourceView**)&pcmd->TextureId);
                ctx->RSSetScissorRects(1, &r);
                ctx->DrawIndexed(pcmd->ElemCount, idx_offset, vtx_offset);
            }
            idx_offset += pcmd->ElemCount;
        }
        vtx_offset += cmd_list->VtxBuffer.size();
    }

    // Restore modified DX state
    ctx->RSSetScissorRects(old.ScissorRectsCount, old.ScissorRects);
    ctx->RSSetViewports(old.ViewportsCount, old.Viewports);
    ctx->RSSetState(old.RS); if (old.RS) old.RS->Release();
    ctx->OMSetBlendState(old.BlendState, old.BlendFactor, old.SampleMask); if (old.BlendState) old.BlendState->Release();
    ctx->PSSetShaderResources(0, 1, &old.PSShaderResource); if (old.PSShaderResource) old.PSShaderResource->Release();
    ctx->PSSetSamplers(0, 1, &old.PSSampler); if (old.PSSampler) old.PSSampler->Release();
    ctx->PSSetShader(old.PS, old.PSInstances, old.PSInstancesCount); if (old.PS) old.PS->Release();
    for (UINT i = 0; i < old.PSInstancesCount; i++) if (old.PSInstances[i]) old.PSInstances[i]->Release();
    ctx->VSSetShader(old.VS, old.VSInstances, old.VSInstancesCount); if (old.VS) old.VS->Release();
    ctx->VSSetConstantBuffers(0, 1, &old.VSConstantBuffer); if (old.VSConstantBuffer) old.VSConstantBuffer->Release();
    for (UINT i = 0; i < old.VSInstancesCount; i++) if (old.VSInstances[i]) old.VSInstances[i]->Release();
    ctx->IASetPrimitiveTopology(old.PrimitiveTopology);
    ctx->IASetIndexBuffer(old.IndexBuffer, old.IndexBufferFormat, old.IndexBufferOffset); if (old.IndexBuffer) old.IndexBuffer->Release();
    ctx->IASetVertexBuffers(0, 1, &old.VertexBuffer, &old.VertexBufferStride, &old.VertexBufferOffset); if (old.VertexBuffer) old.VertexBuffer->Release();
    ctx->IASetInputLayout(old.InputLayout); if (old.InputLayout) old.InputLayout->Release();
}

IMGUI_API LRESULT ImGui_ImplDX11_WndProcHandler(HWND, UINT msg, WPARAM wParam, LPARAM lParam)
{
    ImGuiIO& io = ImGui::GetIO();
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        io.MouseDown[0] = true;
        return true;
    case WM_LBUTTONUP:
        io.MouseDown[0] = false;
        return true;
    case WM_RBUTTONDOWN:
        io.MouseDown[1] = true;
        return true;
    case WM_RBUTTONUP:
        io.MouseDown[1] = false;
        return true;
    case WM_MBUTTONDOWN:
        io.MouseDown[2] = true;
        return true;
    case WM_MBUTTONUP:
        io.MouseDown[2] = false;
        return true;
    case WM_MOUSEWHEEL:
        io.MouseWheel += GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? +1.0f : -1.0f;
        return true;
    case WM_MOUSEMOVE:
        io.MousePos.x = (signed short)(lParam);
        io.MousePos.y = (signed short)(lParam >> 16);
        return true;
    case WM_KEYDOWN:
        if (wParam < 256)
            io.KeysDown[wParam] = 1;
        return true;
    case WM_KEYUP:
        if (wParam < 256)
            io.KeysDown[wParam] = 0;
        return true;
    case WM_CHAR:
        // You can also use ToAscii()+GetKeyboardState() to retrieve characters.
        if (wParam > 0 && wParam < 0x10000)
            io.AddInputCharacter((unsigned short)wParam);
        return true;
    }
    return 0;
}

static void ImGui_ImplDX11_CreateFontsTexture()
{
    // Build texture atlas
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    // Upload texture to graphics system
    {
        D3D11_TEXTURE2D_DESC desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;

        ID3D11Texture2D *pTexture = NULL;
        D3D11_SUBRESOURCE_DATA subResource;
        subResource.pSysMem = pixels;
        subResource.SysMemPitch = desc.Width * 4;
        subResource.SysMemSlicePitch = 0;
        g_pd3dDevice->CreateTexture2D(&desc, &subResource, &pTexture);

        // Create texture view
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
        ZeroMemory(&srvDesc, sizeof(srvDesc));
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = desc.MipLevels;
        srvDesc.Texture2D.MostDetailedMip = 0;
        g_pd3dDevice->CreateShaderResourceView(pTexture, &srvDesc, &g_pFontTextureView);
        pTexture->Release();
    }

    // Store our identifier
    io.Fonts->TexID = (void *)g_pFontTextureView;

    // Create texture sampler
    {
        D3D11_SAMPLER_DESC desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.MipLODBias = 0.f;
        desc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        desc.MinLOD = 0.f;
        desc.MaxLOD = 0.f;
        g_pd3dDevice->CreateSamplerState(&desc, &g_pFontSampler);
    }
}

bool    ImGui_ImplDX11_CreateDeviceObjects()
{
    if (!g_pd3dDevice)
        return false;
    if (g_pFontSampler)
        ImGui_ImplDX11_InvalidateDeviceObjects();

    // Create the vertex shader
    {

		static const BYTE g_vs30_main[] =
		{
			68,  88,  66,  67, 248, 206,
			245,  11,  44,   6,  99, 108,
			249,  25,  79, 173, 243, 206,
			116,  52,   1,   0,   0,   0,
			120,   3,   0,   0,   5,   0,
			0,   0,  52,   0,   0,   0,
			16,   1,   0,   0, 128,   1,
			0,   0, 244,   1,   0,   0,
			252,   2,   0,   0,  82,  68,
			69,  70, 212,   0,   0,   0,
			1,   0,   0,   0,  76,   0,
			0,   0,   1,   0,   0,   0,
			28,   0,   0,   0,   0,   4,
			254, 255,   0,   1,   0,   0,
			160,   0,   0,   0,  60,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   1,   0,
			0,   0,   1,   0,   0,   0,
			118, 101, 114, 116, 101, 120,
			66, 117, 102, 102, 101, 114,
			0, 171, 171, 171,  60,   0,
			0,   0,   1,   0,   0,   0,
			100,   0,   0,   0,  64,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0, 124,   0,
			0,   0,   0,   0,   0,   0,
			64,   0,   0,   0,   2,   0,
			0,   0, 144,   0,   0,   0,
			0,   0,   0,   0,  80, 114,
			111, 106, 101,  99, 116, 105,
			111, 110,  77,  97, 116, 114,
			105, 120,   0, 171, 171, 171,
			3,   0,   3,   0,   4,   0,
			4,   0,   0,   0,   0,   0,
			0,   0,   0,   0,  77, 105,
			99, 114, 111, 115, 111, 102,
			116,  32,  40,  82,  41,  32,
			72,  76,  83,  76,  32,  83,
			104,  97, 100, 101, 114,  32,
			67, 111, 109, 112, 105, 108,
			101, 114,  32,  54,  46,  51,
			46,  57,  54,  48,  48,  46,
			49,  54,  51,  56,  52,   0,
			171, 171,  73,  83,  71,  78,
			104,   0,   0,   0,   3,   0,
			0,   0,   8,   0,   0,   0,
			80,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			3,   0,   0,   0,   0,   0,
			0,   0,   3,   3,   0,   0,
			89,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			3,   0,   0,   0,   1,   0,
			0,   0,  15,  15,   0,   0,
			95,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			3,   0,   0,   0,   2,   0,
			0,   0,   3,   3,   0,   0,
			80,  79,  83,  73,  84,  73,
			79,  78,   0,  67,  79,  76,
			79,  82,   0,  84,  69,  88,
			67,  79,  79,  82,  68,   0,
			79,  83,  71,  78, 108,   0,
			0,   0,   3,   0,   0,   0,
			8,   0,   0,   0,  80,   0,
			0,   0,   0,   0,   0,   0,
			1,   0,   0,   0,   3,   0,
			0,   0,   0,   0,   0,   0,
			15,   0,   0,   0,  92,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   3,   0,
			0,   0,   1,   0,   0,   0,
			15,   0,   0,   0,  98,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   3,   0,
			0,   0,   2,   0,   0,   0,
			3,  12,   0,   0,  83,  86,
			95,  80,  79,  83,  73,  84,
			73,  79,  78,   0,  67,  79,
			76,  79,  82,   0,  84,  69,
			88,  67,  79,  79,  82,  68,
			0, 171,  83,  72,  68,  82,
			0,   1,   0,   0,  64,   0,
			1,   0,  64,   0,   0,   0,
			89,   0,   0,   4,  70, 142,
			32,   0,   0,   0,   0,   0,
			4,   0,   0,   0,  95,   0,
			0,   3,  50,  16,  16,   0,
			0,   0,   0,   0,  95,   0,
			0,   3, 242,  16,  16,   0,
			1,   0,   0,   0,  95,   0,
			0,   3,  50,  16,  16,   0,
			2,   0,   0,   0, 103,   0,
			0,   4, 242,  32,  16,   0,
			0,   0,   0,   0,   1,   0,
			0,   0, 101,   0,   0,   3,
			242,  32,  16,   0,   1,   0,
			0,   0, 101,   0,   0,   3,
			50,  32,  16,   0,   2,   0,
			0,   0, 104,   0,   0,   2,
			1,   0,   0,   0,  56,   0,
			0,   8, 242,   0,  16,   0,
			0,   0,   0,   0,  86,  21,
			16,   0,   0,   0,   0,   0,
			70, 142,  32,   0,   0,   0,
			0,   0,   1,   0,   0,   0,
			50,   0,   0,  10, 242,   0,
			16,   0,   0,   0,   0,   0,
			70, 142,  32,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			6,  16,  16,   0,   0,   0,
			0,   0,  70,  14,  16,   0,
			0,   0,   0,   0,   0,   0,
			0,   8, 242,  32,  16,   0,
			0,   0,   0,   0,  70,  14,
			16,   0,   0,   0,   0,   0,
			70, 142,  32,   0,   0,   0,
			0,   0,   3,   0,   0,   0,
			54,   0,   0,   5, 242,  32,
			16,   0,   1,   0,   0,   0,
			70,  30,  16,   0,   1,   0,
			0,   0,  54,   0,   0,   5,
			50,  32,  16,   0,   2,   0,
			0,   0,  70,  16,  16,   0,
			2,   0,   0,   0,  62,   0,
			0,   1,  83,  84,  65,  84,
			116,   0,   0,   0,   6,   0,
			0,   0,   1,   0,   0,   0,
			0,   0,   0,   0,   6,   0,
			0,   0,   3,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   1,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   2,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0
		};


        if (g_pd3dDevice->CreateVertexShader(g_vs30_main, sizeof(g_vs30_main), NULL, &g_pVertexShader) != S_OK)
            return false;

        // Create the input layout
        D3D11_INPUT_ELEMENT_DESC local_layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, (size_t)(&((ImDrawVert*)0)->pos), D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, (size_t)(&((ImDrawVert*)0)->uv),  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (size_t)(&((ImDrawVert*)0)->col), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (g_pd3dDevice->CreateInputLayout(local_layout, 3, g_vs30_main, sizeof(g_vs30_main), &g_pInputLayout) != S_OK)
            return false;

        // Create the constant buffer
        {
            D3D11_BUFFER_DESC desc;
            desc.ByteWidth = sizeof(VERTEX_CONSTANT_BUFFER);
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.MiscFlags = 0;
            g_pd3dDevice->CreateBuffer(&desc, NULL, &g_pVertexConstantBuffer);
        }
    }

    // Create the pixel shader
    {
        static const BYTE g_ps30_main[] =
		{
			68,  88,  66,  67, 129, 206,
			12,  70,  68,  73,  37, 152,
			201,  75,  86, 151, 203,  28,
			142, 122,   1,   0,   0,   0,
			156,   2,   0,   0,   5,   0,
			0,   0,  52,   0,   0,   0,
			220,   0,   0,   0,  80,   1,
			0,   0, 132,   1,   0,   0,
			32,   2,   0,   0,  82,  68,
			69,  70, 160,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   2,   0,   0,   0,
			28,   0,   0,   0,   0,   4,
			255, 255,   0,   1,   0,   0,
			110,   0,   0,   0,  92,   0,
			0,   0,   3,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   1,   0,
			0,   0,   0,   0,   0,   0,
			101,   0,   0,   0,   2,   0,
			0,   0,   5,   0,   0,   0,
			4,   0,   0,   0, 255, 255,
			255, 255,   0,   0,   0,   0,
			1,   0,   0,   0,  12,   0,
			0,   0, 115,  97, 109, 112,
			108, 101, 114,  48,   0, 116,
			101, 120, 116, 117, 114, 101,
			48,   0,  77, 105,  99, 114,
			111, 115, 111, 102, 116,  32,
			40,  82,  41,  32,  72,  76,
			83,  76,  32,  83, 104,  97,
			100, 101, 114,  32,  67, 111,
			109, 112, 105, 108, 101, 114,
			32,  54,  46,  51,  46,  57,
			54,  48,  48,  46,  49,  54,
			51,  56,  52,   0,  73,  83,
			71,  78, 108,   0,   0,   0,
			3,   0,   0,   0,   8,   0,
			0,   0,  80,   0,   0,   0,
			0,   0,   0,   0,   1,   0,
			0,   0,   3,   0,   0,   0,
			0,   0,   0,   0,  15,   0,
			0,   0,  92,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   3,   0,   0,   0,
			1,   0,   0,   0,  15,  15,
			0,   0,  98,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   3,   0,   0,   0,
			2,   0,   0,   0,   3,   3,
			0,   0,  83,  86,  95,  80,
			79,  83,  73,  84,  73,  79,
			78,   0,  67,  79,  76,  79,
			82,   0,  84,  69,  88,  67,
			79,  79,  82,  68,   0, 171,
			79,  83,  71,  78,  44,   0,
			0,   0,   1,   0,   0,   0,
			8,   0,   0,   0,  32,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   3,   0,
			0,   0,   0,   0,   0,   0,
			15,   0,   0,   0,  83,  86,
			95,  84,  97, 114, 103, 101,
			116,   0, 171, 171,  83,  72,
			68,  82, 148,   0,   0,   0,
			64,   0,   0,   0,  37,   0,
			0,   0,  90,   0,   0,   3,
			0,  96,  16,   0,   0,   0,
			0,   0,  88,  24,   0,   4,
			0, 112,  16,   0,   0,   0,
			0,   0,  85,  85,   0,   0,
			98,  16,   0,   3, 242,  16,
			16,   0,   1,   0,   0,   0,
			98,  16,   0,   3,  50,  16,
			16,   0,   2,   0,   0,   0,
			101,   0,   0,   3, 242,  32,
			16,   0,   0,   0,   0,   0,
			104,   0,   0,   2,   1,   0,
			0,   0,  69,   0,   0,   9,
			242,   0,  16,   0,   0,   0,
			0,   0,  70,  16,  16,   0,
			2,   0,   0,   0,  70, 126,
			16,   0,   0,   0,   0,   0,
			0,  96,  16,   0,   0,   0,
			0,   0,  56,   0,   0,   7,
			242,  32,  16,   0,   0,   0,
			0,   0,  70,  14,  16,   0,
			0,   0,   0,   0,  70,  30,
			16,   0,   1,   0,   0,   0,
			62,   0,   0,   1,  83,  84,
			65,  84, 116,   0,   0,   0,
			3,   0,   0,   0,   1,   0,
			0,   0,   0,   0,   0,   0,
			3,   0,   0,   0,   1,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   1,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   1,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0,   0,   0,   0,   0,
			0,   0
		};

        if (g_pd3dDevice->CreatePixelShader(g_ps30_main, sizeof(g_ps30_main), NULL, &g_pPixelShader) != S_OK)
            return false;
    }

    // Create the blending setup
    {
        D3D11_BLEND_DESC desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.AlphaToCoverageEnable = false;
        desc.RenderTarget[0].BlendEnable = true;
        desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_pd3dDevice->CreateBlendState(&desc, &g_pBlendState);
    }

    // Create the rasterizer state
    {
        D3D11_RASTERIZER_DESC desc;
        ZeroMemory(&desc, sizeof(desc));
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        desc.ScissorEnable = true;
        desc.DepthClipEnable = true;
        g_pd3dDevice->CreateRasterizerState(&desc, &g_pRasterizerState);
    }

    ImGui_ImplDX11_CreateFontsTexture();

    return true;
}

void    ImGui_ImplDX11_InvalidateDeviceObjects()
{
    if (!g_pd3dDevice)
        return;

    if (g_pFontSampler) { g_pFontSampler->Release(); g_pFontSampler = NULL; }
    if (g_pFontTextureView) { g_pFontTextureView->Release(); g_pFontTextureView = NULL; ImGui::GetIO().Fonts->TexID = 0; }
    if (g_pIB) { g_pIB->Release(); g_pIB = NULL; }
    if (g_pVB) { g_pVB->Release(); g_pVB = NULL; }

    if (g_pBlendState) { g_pBlendState->Release(); g_pBlendState = NULL; }
    if (g_pRasterizerState) { g_pRasterizerState->Release(); g_pRasterizerState = NULL; }
    if (g_pPixelShader) { g_pPixelShader->Release(); g_pPixelShader = NULL; }
    if (g_pVertexConstantBuffer) { g_pVertexConstantBuffer->Release(); g_pVertexConstantBuffer = NULL; }
    if (g_pInputLayout) { g_pInputLayout->Release(); g_pInputLayout = NULL; }
    if (g_pVertexShader) { g_pVertexShader->Release(); g_pVertexShader = NULL; }
}

bool    ImGui_ImplDX11_Init(void* hwnd, ID3D11Device* device, ID3D11DeviceContext* device_context)
{
    g_hWnd = (HWND)hwnd;
    g_pd3dDevice = device;
    g_pd3dDeviceContext = device_context;

    if (!QueryPerformanceFrequency((LARGE_INTEGER *)&g_TicksPerSecond))
        return false;
    if (!QueryPerformanceCounter((LARGE_INTEGER *)&g_Time))
        return false;

    ImGuiIO& io = ImGui::GetIO();
    io.KeyMap[ImGuiKey_Tab] = VK_TAB;                       // Keyboard mapping. ImGui will use those indices to peek into the io.KeyDown[] array that we will update during the application lifetime.
    io.KeyMap[ImGuiKey_LeftArrow] = VK_LEFT;
    io.KeyMap[ImGuiKey_RightArrow] = VK_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow] = VK_UP;
    io.KeyMap[ImGuiKey_DownArrow] = VK_DOWN;
    io.KeyMap[ImGuiKey_PageUp] = VK_PRIOR;
    io.KeyMap[ImGuiKey_PageDown] = VK_NEXT;
    io.KeyMap[ImGuiKey_Home] = VK_HOME;
    io.KeyMap[ImGuiKey_End] = VK_END;
    io.KeyMap[ImGuiKey_Delete] = VK_DELETE;
    io.KeyMap[ImGuiKey_Backspace] = VK_BACK;
    io.KeyMap[ImGuiKey_Enter] = VK_RETURN;
    io.KeyMap[ImGuiKey_Escape] = VK_ESCAPE;
    io.KeyMap[ImGuiKey_A] = 'A';
    io.KeyMap[ImGuiKey_C] = 'C';
    io.KeyMap[ImGuiKey_V] = 'V';
    io.KeyMap[ImGuiKey_X] = 'X';
    io.KeyMap[ImGuiKey_Y] = 'Y';
    io.KeyMap[ImGuiKey_Z] = 'Z';

    io.RenderDrawListsFn = ImGui_ImplDX11_RenderDrawLists;  // Alternatively you can set this to NULL and call ImGui::GetDrawData() after ImGui::Render() to get the same ImDrawData pointer.
    io.ImeWindowHandle = g_hWnd;

    return true;
}

void ImGui_ImplDX11_Shutdown()
{
    ImGui_ImplDX11_InvalidateDeviceObjects();
    ImGui::Shutdown();
    g_pd3dDevice = NULL;
    g_pd3dDeviceContext = NULL;
    g_hWnd = (HWND)0;
}

void ImGui_ImplDX11_NewFrame()
{
    if (!g_pFontSampler)
        ImGui_ImplDX11_CreateDeviceObjects();

    ImGuiIO& io = ImGui::GetIO();

    // Setup display size (every frame to accommodate for window resizing)
    RECT rect;
    GetClientRect(g_hWnd, &rect);
    io.DisplaySize = ImVec2((float)(rect.right - rect.left), (float)(rect.bottom - rect.top));

    // Setup time step
    INT64 current_time;
    QueryPerformanceCounter((LARGE_INTEGER *)&current_time);
    io.DeltaTime = (float)(current_time - g_Time) / g_TicksPerSecond;
    g_Time = current_time;

    // Start the frame
    ImGui::NewFrame();
}
