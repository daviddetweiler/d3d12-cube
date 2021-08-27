cbuffer buffer : register(b0)
{
	float4x4 view;
	float4x4 projection;
}

float4 main(float3 position : POSITION) : SV_POSITION { return float4(position, 1.0); }
