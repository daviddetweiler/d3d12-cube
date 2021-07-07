struct constant_data {
	row_major float4x4 projection;
	row_major float4x4 view;
};

struct vertex_output {
	float3 color : COLOR;
	float4 position : SV_POSITION;
};

ConstantBuffer<constant_data> constants : register(b0);

[RootSignature("RootConstants(num32BitConstants=32,b0), RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)")] vertex_output
main(float3 position
	 : SV_POSITION) {
	vertex_output output;
	output.position = mul(mul(float4(position, 1.0f), constants.view), constants.projection);
	output.color = position;
	return output;
}