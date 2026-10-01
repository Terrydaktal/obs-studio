#include "../obs-ffmpeg-video-encoders.h"

#include <stdio.h>
#include <stdlib.h>

/* Exercise the production encode callback with deterministic packet timing.
 * No real-time sleeps or overloaded workstation are needed to reproduce the
 * old failure, including a delayed first packet and a keyframe-sized burst. */
static uint64_t now_ns;
static uint64_t pause_ns;
static int64_t packet_pts;
static int send_result;
static int receive_result;
static unsigned warnings;
static unsigned recoveries;
static unsigned errors;
static unsigned last_errors;

#define CHECK(condition)                                                           \
	do {                                                                       \
		if (!(condition)) {                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
			exit(1);                                                   \
		}                                                                  \
	} while (0)

static int mock_send_frame(AVCodecContext *context, const AVFrame *frame)
{
	(void)context;
	(void)frame;
	return send_result;
}

static int mock_receive_packet(AVCodecContext *context, AVPacket *packet)
{
	(void)context;
	if (receive_result)
		return receive_result;
	CHECK(av_new_packet(packet, 4) == 0);
	memcpy(packet->data, "AV1!", 4);
	packet->pts = packet_pts;
	packet->dts = packet_pts;
	packet->flags = AV_PKT_FLAG_KEY;
	return 0;
}

static uint64_t mock_time(void)
{
	return now_ns;
}

static uint64_t mock_pause(const obs_encoder_t *encoder)
{
	(void)encoder;
	return pause_ns;
}

static const char *mock_name(const obs_encoder_t *encoder)
{
	(void)encoder;
	return "test_video_recording";
}

static void mock_last_error(obs_encoder_t *encoder, const char *error)
{
	(void)encoder;
	CHECK(error != NULL);
	last_errors++;
}

static const char *mock_text(const char *key)
{
	(void)key;
	return "Encoder %1 timeout %2 seconds";
}

static void mock_log(int level, const char *format, ...)
{
	if (level == LOG_WARNING)
		warnings++;
	if (level == LOG_ERROR)
		errors++;
	if (level == LOG_INFO && strstr(format, "delay recovered"))
		recoveries++;
}

#define avcodec_send_frame mock_send_frame
#define avcodec_receive_packet mock_receive_packet
#define os_gettime_ns mock_time
#define obs_encoder_get_pause_offset mock_pause
#define obs_encoder_get_name mock_name
#define obs_encoder_set_last_error mock_last_error
#define obs_module_text mock_text
#define blog mock_log
#include "../obs-ffmpeg-video-encoders.c"

static bool encode_at_delay(struct ffmpeg_video_encoder *enc, int64_t delay_ns, bool *received)
{
	struct encoder_frame frame = {0};
	struct encoder_packet packet = {0};
	now_ns = enc->start_ts + packet_pts * SEC_TO_NSEC / 30 + delay_ns + pause_ns;
	bool success = ffmpeg_video_encode(enc, &frame, &packet, received);
	if (*received) {
		CHECK(packet.pts == packet_pts);
		CHECK(packet.dts == packet_pts);
		CHECK(packet.size == 4);
		CHECK(memcmp(packet.data, "AV1!", 4) == 0);
		CHECK(packet.keyframe);
	}
	return success;
}

int main(void)
{
	AVCodecContext *context = avcodec_alloc_context3(NULL);
	AVFrame *vframe = av_frame_alloc();
	CHECK(context && vframe);
	context->time_base = (AVRational){1, 30};
	context->pix_fmt = AV_PIX_FMT_YUV420P;
	struct ffmpeg_video_encoder enc = {
		.enc_name = "SVT-AV1",
		.context = context,
		.vframe = vframe,
		.start_ts = 10 * SEC_TO_NSEC,
		.first_packet = true,
		.allow_delayed_output = true,
	};
	bool received = false;

	/* The first packet must survive even when initial buffering takes >5 s. */
	CHECK(encode_at_delay(&enc, 6 * SEC_TO_NSEC, &received));
	CHECK(received && warnings == 1 && last_errors == 0 && errors == 0);
	/* Sustained delay must not stop recording or spam one warning per frame. */
	for (packet_pts = 1; packet_pts <= 1800; packet_pts++) {
		CHECK(encode_at_delay(&enc, 8 * SEC_TO_NSEC, &received));
		CHECK(received);
	}
	CHECK(warnings == 1 && last_errors == 0);
	CHECK(encode_at_delay(&enc, 4 * SEC_TO_NSEC, &received));
	CHECK(recoveries == 0);
	CHECK(encode_at_delay(&enc, 2 * SEC_TO_NSEC, &received));
	CHECK(recoveries == 1);
	CHECK(encode_at_delay(&enc, 6 * SEC_TO_NSEC, &received));
	CHECK(warnings == 2);

	/* Streaming/default policy still fails at the original threshold. */
	enc.allow_delayed_output = false;
	CHECK(encode_at_delay(&enc, 5 * SEC_TO_NSEC, &received));
	CHECK(!encode_at_delay(&enc, 6 * SEC_TO_NSEC, &received));
	CHECK(errors == 1 && last_errors == 1);
	pause_ns = 20 * SEC_TO_NSEC;
	CHECK(encode_at_delay(&enc, SEC_TO_NSEC, &received));
	CHECK(errors == 1 && last_errors == 1);

	/* The recording policy must not conceal real codec errors or invent output. */
	enc.allow_delayed_output = true;
	receive_result = AVERROR(EAGAIN);
	CHECK(encode_at_delay(&enc, 6 * SEC_TO_NSEC, &received));
	CHECK(!received);
	send_result = AVERROR(EIO);
	CHECK(!encode_at_delay(&enc, 6 * SEC_TO_NSEC, &received));

	da_free(enc.buffer);
	av_frame_free(&vframe);
	avcodec_free_context(&context);
	puts("PASS: delayed recording packets, warning recovery, default timeout, pause, codec errors");
	return 0;
}
