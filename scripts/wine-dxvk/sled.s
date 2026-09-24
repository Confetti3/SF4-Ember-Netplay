# No-op code covering every SSFIV.exe offset Sidecar hooks (highest is +0x6a9268).
# run.sh fails if the hooks do not commit, which is the sign to grow the fill.
	.text
	.globl _sled
_sled:
	.fill 0x6c0000,1,0x90
	ret
