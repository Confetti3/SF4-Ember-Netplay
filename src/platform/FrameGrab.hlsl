// The frame grab's two passes (FrameGrab.hxx), compiled when Ember is built
// (CMakeLists.txt) into the arrays FrameGrab.cxx hands the device.
// t is the middle of the output pixel on the source: four source columns
// wide in both passes, one row tall for luma and two for chroma. d is one
// source pixel. A8R8G8B8 lies in memory as B, G, R, A, so the first byte of
// each four is the colour's blue.
sampler source : register(s0);
float4 d : register(c0);
float Y(float2 t) { return dot(tex2D(source, t).rgb, float3(0.1826, 0.6142, 0.0620)) + 16.0 / 255; }
float2 UV(float2 t) {
	float3 c = tex2D(source, t).rgb;
	return float2(dot(c, float3(-0.1006, -0.3386, 0.4392)), dot(c, float3(0.4392, -0.3989, -0.0403))) + 128.0 / 255;
}
float4 Luma(float2 t : TEXCOORD0) : COLOR {
	return float4(Y(t + float2(0.5 * d.x, 0)), Y(t - float2(0.5 * d.x, 0)), Y(t - float2(1.5 * d.x, 0)), Y(t + float2(1.5 * d.x, 0)));
}
// Sampled between four pixels with linear filtering: their average.
float4 Chroma(float2 t : TEXCOORD0) : COLOR {
	float2 left = UV(t - float2(d.x, 0)), right = UV(t + float2(d.x, 0));
	return float4(right.x, left.y, left.x, right.y);
}
