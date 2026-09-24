# No-op code covering every SSFIV.exe offset Sidecar hooks (highest is +0x6a9268).
	.text
	.globl _sled
_sled:
	.fill 0x6c0000,1,0x90
	ret
