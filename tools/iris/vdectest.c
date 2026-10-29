/*
 * vdectest: decode a video file on Android with a named MediaCodec decoder, from a root shell, no window.
 *
 *   vdectest FILE [CODEC [FRAME0_OUT]]
 *
 * CODEC defaults to c2.qti.hevc.decoder (Qualcomm's Iris decoder). Each output frame prints its index, size and an
 * FNV-1a hash of its bytes; the first frame's bytes go to FRAME0_OUT when given, with the output format printed so
 * its layout (width, height, stride, slice height, colour format) is known. On Android this drives the video core
 * the way an app does, so its power-up, firmware load and packets can be traced (tools/iris/README.md).
 */
#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static uint64_t fnv1a(const uint8_t *p, size_t n)
{
	uint64_t h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ull;
	}
	return h;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: vdectest FILE [CODEC [FRAME0_OUT]]\n");
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);       /* a run that hangs still shows how far it got */
	const char *codec_name = argc > 2 ? argv[2] : "c2.qti.hevc.decoder";
	const char *frame0 = argc > 3 ? argv[3] : NULL;
	int fd = open(argv[1], O_RDONLY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) != 0) {
		perror(argv[1]);
		return 1;
	}
	AMediaExtractor *ex = AMediaExtractor_new();
	if (AMediaExtractor_setDataSourceFd(ex, fd, 0, st.st_size) != AMEDIA_OK) {
		fprintf(stderr, "extractor refused %s\n", argv[1]);
		return 1;
	}
	int track = -1;
	AMediaFormat *in_format = NULL;
	for (size_t i = 0; i < AMediaExtractor_getTrackCount(ex); i++) {
		AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, i);
		const char *mime = NULL;
		if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &mime) && strncmp(mime, "video/", 6) == 0) {
			track = (int)i;
			in_format = f;
			break;
		}
		AMediaFormat_delete(f);
	}
	if (track < 0) {
		fprintf(stderr, "no video track\n");
		return 1;
	}
	printf("input: %s\n", AMediaFormat_toString(in_format));
	AMediaExtractor_selectTrack(ex, track);

	const uint64_t t0 = now_us();
	AMediaCodec *codec = AMediaCodec_createCodecByName(codec_name);
	if (codec == NULL) {
		fprintf(stderr, "no codec %s\n", codec_name);
		return 1;
	}
	media_status_t rc = AMediaCodec_configure(codec, in_format, NULL, NULL, 0);
	if (rc != AMEDIA_OK || (rc = AMediaCodec_start(codec)) != AMEDIA_OK) {
		fprintf(stderr, "codec %s: configure/start failed %d\n", codec_name, rc);
		return 1;
	}
	printf("codec %s started in %llu us\n", codec_name, (unsigned long long)(now_us() - t0));

	int input_done = 0, frames = 0, inputs = 0;
	uint64_t first_out = 0;
	int32_t width = 0, height = 0, stride = 0, slice = 0, color = 0;
	while (1) {
		if (!input_done) {
			ssize_t ib = AMediaCodec_dequeueInputBuffer(codec, 10000);
			if (ib >= 0) {
				size_t cap = 0;
				uint8_t *buf = AMediaCodec_getInputBuffer(codec, ib, &cap);
				ssize_t n = AMediaExtractor_readSampleData(ex, buf, cap);
				if (n < 0) {
					AMediaCodec_queueInputBuffer(codec, ib, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
					input_done = 1;
					printf("end of stream queued after %d inputs\n", inputs);
				} else {
					printf("input %d: %zd bytes\n", inputs, n);
					AMediaCodec_queueInputBuffer(codec, ib, 0, n, AMediaExtractor_getSampleTime(ex), 0);
					AMediaExtractor_advance(ex);
					inputs++;
				}
			}
		}
		AMediaCodecBufferInfo info;
		ssize_t ob = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
		if (ob == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
			AMediaFormat *f = AMediaCodec_getOutputFormat(codec);
			AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_WIDTH, &width);
			AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_HEIGHT, &height);
			AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_STRIDE, &stride);
			AMediaFormat_getInt32(f, "slice-height", &slice);
			AMediaFormat_getInt32(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, &color);
			printf("output format: %s\n", AMediaFormat_toString(f));
			AMediaFormat_delete(f);
			continue;
		}
		if (ob < 0) {
			if (now_us() - t0 > 20000000) {
				printf("timed out after 20 s: %d inputs queued%s, %d frames out\n", inputs,
				    input_done ? " and the end of stream" : "", frames);
				break;
			}
			continue;
		}
		size_t cap = 0;
		uint8_t *out = AMediaCodec_getOutputBuffer(codec, ob, &cap);
		if (info.size > 0 && out != NULL) {
			if (frames == 0) {
				first_out = now_us() - t0;
				if (frame0 != NULL) {
					FILE *fp = fopen(frame0, "wb");
					if (fp != NULL) {
						fwrite(out + info.offset, 1, info.size, fp);
						fclose(fp);
					}
				}
			}
			printf("frame %d pts %lld size %d hash %016llx\n", frames, (long long)info.presentationTimeUs, info.size,
			    (unsigned long long)fnv1a(out + info.offset, info.size));
			frames++;
		}
		AMediaCodec_releaseOutputBuffer(codec, ob, false);
		if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) {
			break;
		}
	}
	printf("%d inputs, %d frames %dx%d stride %d slice %d colour %d; first frame after %llu us, all after %llu us\n",
	    inputs, frames, width, height, stride, slice, color, (unsigned long long)first_out,
	    (unsigned long long)(now_us() - t0));
	AMediaCodec_stop(codec);
	AMediaCodec_delete(codec);
	AMediaExtractor_delete(ex);
	close(fd);
	return frames > 0 ? 0 : 1;
}
