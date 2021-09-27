struct vertex {
	float4 position : SV_POSITION;
	float3 color : COLOR;
};

float4 main(vertex data) : SV_TARGET { return float4(data.color, 1.0f); }