/* The part of the texture this viewport shows: texcoord = offset + uv * scale.
 * (0, 0, 1, 1) maps the whole texture; a viewport clamped to the render target
 * keeps the visible part of a rect that overhangs it. */
cbuffer FullscreenVS : register(b0, space1)
{
	float4 uv_rect; /* xy offset, zw scale */
};

struct VertexOutput {
	float4 position : SV_Position;
	float2 texcoord : TEXCOORD0;
};

VertexOutput main(uint vertexId : SV_VertexID) {
	VertexOutput output;
	float2       positions[3] = {
		float2(-1.0, -1.0),
		float2(-1.0, 3.0),
		float2(3.0, -1.0),
	};
	float2 texcoords[3] = {
		float2(0.0, 1.0),
		float2(0.0, -1.0),
		float2(2.0, 1.0),
	};

	output.position = float4(positions[vertexId], 0.0, 1.0);
	output.texcoord = uv_rect.xy + texcoords[vertexId] * uv_rect.zw;
	return output;
}
