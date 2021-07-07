#define ROOT_SIGNATURE [RootSignature("")]

ROOT_SIGNATURE float4 main(uint id : SV_VertexID) : SV_POSITION
{
	const float2 vertex[] = {float2(0.0f, 0.0f), float2(0.0f, 0.5f), float2(0.5f, 0.0f)};
	return float4(vertex[id], 0.1f, 1.0f);
}
