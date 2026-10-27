MT6580 HACC software-key AES
===========================

Source and hardware evidence
----------------------------

The reference is android_kernel_mediatek_k80/drivers/misc/mediatek/masp/
asfv2/mach/{hacc_sk.c,hacc_mach.h}, the old MT6580 DT and
base/power/mt6580/mt_clkmgr.c plus its mt_clkmgr.h. The block is at
0x1000a000. ACON/ACON2/ACONK are +4/+8/+c, source is +0x10, software key
+0x20, IV +0x40 and output +0x50. ENC is bit0, CBC bit1, key size bits5:4,
byte order bit12, START bit0, CLR bit1 and READY bit15 in ACON2.

A live #161 hardware probe reproduced AES-128 encryption. Using the BSP's
wrapping-key byte conversion on a raw Linux key instead encrypted with each
key word reversed. Little-endian software key, source/IV and output words,
with byte swapping disabled, match standard AES. The public 000102..0f key
and 001122..ff input produce 69c4e0d86a7b0430d8cdb78070b4c55a. A different
fresh input also matched an independent OpenSSL result with CG_CTRL2's
SEJ_13M/AES gates both disabled. The necessary bus gate is SEJ_SW_CG,
TOP CG_CTRL1 at 0x24 bit21, documented in the BSP clock manager.

Only that gate is connected to the new consumer. Its physical two-input
clock route/rate is not established, so it is a gate without a fabricated
PLL parent or frequency. Work is drained and this gate closed before
system deep may change AXI/MAINPLL. Other security/AES gates retain their
existing ownership. The probe restores its original gate bits afterward.

TRNG is a separate unresolved block. The BSP supplies its 0x1020c000 address,
IRQ and CG_CTRL2 bit5, but no implemented register protocol was found in
this source. Neither MT7623 register compatibility nor entropy quality is
inferred from those names; this change registers no hardware RNG.

Driver behavior
---------------

CONFIG_CRYPTO_DEV_MT6580_HACC provides asynchronous ecb(aes)/cbc(aes)
implementations, named mt6580-ecb-aes and mt6580-cbc-aes. Software keys of
128/192/256 bits are accepted. A probe-time hardware AES-128 encrypt/decrypt
known-answer check must pass before registration. Crypto Engine serializes
queued requests; the worker, not an atomic caller, performs PM/clock and PIO
access. The callback converts the asynchronous request base to its enclosing
skcipher_request. The #163 qualification exposed an incorrect direct cast;
its Oops is retained, and no result from that image is counted as success.

The skcipher walker handles scatterlists and unaligned buffers. CBC retains
chaining across blocks/walk fragments and saves source ciphertext before
in-place decryption, updating the request IV correctly. A current bounce
chunk is zeroed before aborting a failed walk so no uninitialized tail is
copied to the caller. Failed requests retain their errors and are not
replayed. Each READY wait is bounded to 1 ms; timeout latches the engine
unavailable until reprobe/reboot, including across runtime/system PM.

Only caller-supplied software keys are used. CON, SW_OTP, SECINIT and MKJ
are never accessed, and BK2C/R2K remain zero. Hardware software-key, source
and IV registers are cleared after each request while the clock is on;
the normal transform key copy remains until setkey/transform teardown and
is explicitly zeroed there. The legacy security device/secure-boot interface
and hardware-derived key operations are not reproduced.

Runtime PM gates the bus after 100 ms idle. System suspend drains the
engine queue before forcing runtime suspend; resume restores PM before
accepting new engine work. Removal/shutdown also drain work before cleanup.
Read-only state reports counters and the timeout latch, without reading
clock-gated MMIO or exposing any data/key registers.

Qualification and selection
---------------------------

Build #164 enabled CRYPTO_SELFTESTS_FULL for qualification. Both algorithms
passed kernel selftests, including in-place/scatterlist/IV cases. The static
AF_ALG fixture also compared all three key sizes, ECB/CBC, six lengths,
fragmented iovecs and unaligned buffers against explicitly selected software
AES. Four concurrent clients and eight idle/clock-resume cycles completed;
state then reported 5154 requests, 12760192 bytes, 797514 hardware blocks
(including two probe KAT blocks), errors/timeouts0 and PM resumes/suspends12/12.
Idle runtime status was suspended, with the SEJ gate reference count zero.
The separate fixed-frequency timing run is not included in those counters.

At a fixed 1300 MHz, 300 AF_ALG operations per size gave these MiB/s results:

==========  ===========  ===============
Bytes       HACC PIO     Generic software
==========  ===========  ===============
16          0.238        0.838
1024        3.506        12.909
16384       4.388        16.247
==========  ===========  ===============

These include AF_ALG/queue costs, not isolated cipher core timing. Hardware
priority is therefore 50, below software AES priority 100; callers may select
mt6580-ecb-aes or mt6580-cbc-aes explicitly. Hardware support does not imply
a speed improvement. Normal build #165 retains CRYPTO_SELFTESTS and AF_ALG
skcipher support, but not CRYPTO_SELFTESTS_FULL.

Evidence and test fixtures are retained under out/a1260-runtime-pm/hacc.
No electrical power measurement, physical fault injection or certification
is claimed by these functional checks.

The normal #165 image passed another 14400 comparison pairs on four clients
started before two real deep intervals. The hardware queue quiesced for
suspend and all clients completed afterward. Stopped counter durations were
9.313716349 and 9.723902501 seconds, with CPU0/L2 cold2 and matching early
MCUSYS/DCM/EMI recovery. Separate 8 MiB RAM hashes matched. Runtime menu then
completed 1203 CPU0 reset/restores, with 1203 matched clock phases, no failure,
matching RAM and four-CPU recovery. Post-resume AES, Mali 1787 frames/green
readback/GL error0 and aligned 4 MiB direct eMMC write/two reads all passed.

At the end of these checks HACC reported 29250 requests, 119786880 bytes and
7486682 blocks, errors/timeouts0; PM transitions7/7, runtime suspended and
SEJ reference0. Kernel taint and RDMA underflow/abnormal counts were zero.
The two intentional deep cycles had success/fail2/0. Cleanup then allowed
Android to attempt another automatic suspend, canceled during process freeze
by pending eventpoll wakeup: final success/fail2/1, failed_freeze1 and device
suspend/resume failures0. This canceled attempt is retained, not counted as
a third successful sleep or an AES error. Default Windows ADB reconnected
without physical help; test files/trace/alarm/locks were removed, four CPUs,
schedutil604500..1300000, default deep and stay-on0 retained. ADB was returned
to ordinary shell. Standard product boot/kernel now use this #165 image.
