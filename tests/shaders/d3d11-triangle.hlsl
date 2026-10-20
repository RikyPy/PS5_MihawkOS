struct V { float4 p : SV_Position; float3 c : COLOR; };
V vs(uint id : SV_VertexID)
{
    V v;
    float2 p[3] = {float2(0,.8), float2(.8,-.8), float2(-.8,-.8)};
    float3 c[3] = {float3(1,0,0), float3(0,1,0), float3(0,0,1)};
    v.p = float4(p[id],0,1); v.c = c[id]; return v;
}
float4 ps(V v) : SV_Target { return float4(v.c,1); }
