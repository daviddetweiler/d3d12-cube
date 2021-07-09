float4 main(uint id : SV_VertexID) : SV_POSITION
{
	const float3 vertex[] = {float3(0.0f, 0.0f, 0.1f), float3(0.0f, 0.5f, 0.5f), float3(0.5f, 0.0f, 0.3f)};
	return float4(vertex[id], 1.0f);
}
