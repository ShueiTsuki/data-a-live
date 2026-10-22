#!/bin/bash
set -euo pipefail
cd /opt/work/tree
git diff --quiet || { echo "tree dirty"; exit 1; }
python3 - <<'PY'
p = 'bsd/dev/fuxi_keys.c'
t = open(p).read()


def sub(old, new, what):
    global t
    assert t.count(old) == 1, what
    t = t.replace(old, new)


pk = open('/mnt/d/DeepSeekHarness/claude/ios_pandora/wsl/pandora_keys.c.txt').read()
sub("""static void
fk_thread(void *arg, wait_result_t result)
{""", pk + """static void
fk_thread(void *arg, wait_result_t result)
{""", "insert")

sub("""	if (!fk_setup()) {
		goto out;
	}
	for (;;) {
		uint8_t pon = 0, gpio = 1;
		bool pok = fk_read(fk_pon_apid, FK_PON_RT, &pon);
		bool gok = fk_read(fk_gpio_apid, 0x10, &gpio);
		bool ok[3] = {pok, gok, pok};
		int now[3] = {!!(pon & FK_PON_PWR), !(gpio & 1u), !!(pon & FK_PON_RESIN)};
		if ((fk_pon_apid != FK_NO_APID && !pok) ||
		    (fk_gpio_apid != FK_NO_APID && !gok)) {""", """#if defined(PANDORA)
	if (!pk_setup()) {
		goto out;
	}
#else
	if (!fk_setup()) {
		goto out;
	}
#endif
	for (;;) {
		uint8_t pon = 0, gpio = 1;
		bool pok = fk_read(fk_pon_apid, FK_PON_RT, &pon);
#if defined(PANDORA)
		bool up = false, gok = pk_tlmm != 0 && pk_volup(&up);
		gpio = up ? 0 : 1;
		bool gpio_used = pk_tlmm != 0;
#else
		bool gok = fk_read(fk_gpio_apid, 0x10, &gpio);
		bool gpio_used = fk_gpio_apid != FK_NO_APID;
#endif
		bool ok[3] = {pok, gok, pok};
		int now[3] = {!!(pon & FK_PON_PWR), !(gpio & 1u), !!(pon & FK_PON_RESIN)};
		if ((fk_pon_apid != FK_NO_APID && !pok) ||
		    (gpio_used && !gok)) {""", "thread")

sub("""				printf("FUXI KEYS: %s %s (pon=%#x gpio6=%#x)\\n", names[k],""",
    """				printf("FUXI KEYS: %s %s (pon=%#x gpio=%#x)\\n", names[k],""", "log")

# The SM8550 setup is unused on PANDORA.
sub("""static bool
fk_setup(void)
{""", """static bool __unused
fk_setup(void)
{""", "unused")
open(p, 'w').write(t)
PY
git diff --stat
git diff > /mnt/d/DeepSeekHarness/claude/ios_pandora/xnu/patches/0128-pandora-keys-spmi-v8-tlmm.patch
git add -A && git -c user.name=t -c user.email=t@t commit -qm 0128
