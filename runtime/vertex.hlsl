struct constant_data {
	float4x4 projection;
	float4x4 view;
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
	output.position = mul(constants.projection, mul(constants.view, float4(position, 1.0f)));
	output.color = position;
	return output;
}