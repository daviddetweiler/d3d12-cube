struct constant_data {
	matrix projection;
	matrix view;
};

ConstantBuffer<constant_data> constants : register(b0);

[RootSignature("RootConstants(num32BitConstants=32,b0), RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)")] float4
main(float4 position
	 : SV_POSITION) :
	SV_POSITION
{
	return mul(constants.view, position);
}