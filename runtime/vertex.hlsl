cbuffer buffer : register(b0)
{
	row_major float4x4 view;
	float4x4 projection;
}

struct vertex_out {
	float4 position : SV_POSITION;
	float3 color : COLOR;
};

vertex_out main(uint id : SV_VertexID, float3 position : POSITION)
{
	float3 colors[] = {
		float3(1.0f, 0.0f, 0.0f),
		float3(0.0f, 1.0f, 0.0f),
		float3(0.0f, 0.0f, 1.0f)
	};

	vertex_out vertex;
	vertex.position = mul(view, float4(position, 1.0));
	vertex.color = colors[id % 3];
	
	return vertex;
}
