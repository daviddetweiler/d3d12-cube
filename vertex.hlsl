cbuffer buffer : register(b0)
{
	row_major float4x4 view;
	row_major float4x4 projection;
}

struct vertex {
	float4 position : SV_POSITION;
	float3 color : COLOR;
};

vertex main(uint id : SV_VertexID, float3 position : POSITION)
{
	const float3 colors[] = {float3(0.0f, 0.0f, 1.0f), float3(0.0f, 1.0f, 0.0f), float3(1.0f, 0.0f, 0.0f)};
	vertex data;
	data.position = mul(float4(position, 1.0), mul(view, projection));
	data.color = colors[id % 3];
	return data;
}
