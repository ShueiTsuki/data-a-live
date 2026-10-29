"""spk_tunings.py TUNINGS OUT [--orig]: the iPhone X speaker tunings with the speaker-specific effects bypassed, for this phone.

TUNINGS is /Library/Audio/Tunings of the iOS root built from the IPSW (/opt/work/iosroot/ro/root/Library/Audio/
Tunings; Apple's files, kept outside the repo, as OUT is). VirtualAudio builds the speaker's processing from the strips
there (D22 and D221, VAD/d22_speaker_*.austrip: general, alarm, ringtone, movie, latenight_general, latenight_movie,
measurement): DC blocker, virtual bass, compressors, rotation shading, EQs, MeisterStueck, BuzzKill, ControlFreak,
EarPlug, channel delay, volume, AUSpeakerProtectionV3. Apple tuned them to the iPhone X's speakers and enclosure.

On this phone the speakers' sound is tuned by Xiaomi in the audio DSP's graph (DevicePP Audio_MIPLAY, pandora_adsp.c
runs it as Android does) and protected by the CS35L43 amplifiers' firmware with this phone's calibration
(pandora_amp.c). On top of that the iPhone X's strip took up to 10 dB off the speaker for a second or two whenever its
chain was new (made at boot or on a route change) and the music got louder (aud37, aud38, aud40: iOS output against the
music file, 0305 levels; the receiver's chain follows the file within 2 dB). Bypassing AUSpeakerProtectionV3 alone did
not change it (aud40). With every effect but AUDCBlocker and AUVolume bypassed it is gone (aud42; user: 「正常了」).

Each effect in a strip has its own bypass flag (the austrip key "bypass"). This sets it to 1 on every effect except
AUDCBlocker and AUVolume (the system volume) and changes nothing else. OUT gets PRODUCT.NAME.austrip for each strip
(D22.d22_speaker_general.austrip, ...); with --orig the strips unchanged, to put Apple's back.
tools/gpu_test.sh --tunings OUT installs them at /Library/Audio/Tunings/PRODUCT/VAD/NAME.
"""
import os
import plistlib
import sys

KEEP = {'Apple: AUDCBlocker', 'Apple: AUVolume', 'Apple: AUVolume~2'}


def main():
    args = [a for a in sys.argv[1:] if a != '--orig']
    if len(args) != 2:
        sys.exit(__doc__)
    src, out = args
    orig = '--orig' in sys.argv
    os.makedirs(out, exist_ok=True)
    n = 0
    for product in ('D22', 'D221'):
        vad = os.path.join(src, product, 'VAD')
        for name in sorted(os.listdir(vad)):
            if not (name.startswith('d22_speaker_') and name.endswith('.austrip')):
                continue
            with open(os.path.join(vad, name), 'rb') as f:
                strip = plistlib.load(f)
            effects = [e for s in strip['strips'] for e in s['effects']]
            if not any(e.get('displayname') == 'Apple: AUVolume' for e in effects):
                sys.exit(f'{product}/{name}: no AUVolume')
            if not orig:
                for e in effects:
                    if e.get('displayname') not in KEEP:
                        e['bypass'] = 1
            dst = os.path.join(out, f'{product}.{name}')
            assert os.path.dirname(os.path.abspath(dst)) == os.path.abspath(out)
            with open(dst, 'wb') as f:
                plistlib.dump(strip, f, fmt=plistlib.FMT_BINARY)
            n += 1
    print(f'{n} speaker strips to {out}' + (' (unchanged)' if orig else ', all but AUDCBlocker and AUVolume bypassed'))


main()
