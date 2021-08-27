cbuffer buffer : register(b0)
{
	row_major float4x4 view;
	float4x4 projection;
}

float4 main(float3 position : POSITION) : SV_POSITION { return mul(float4(position, 1.0), view); }
