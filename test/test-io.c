/*
 * test-io.c
 * Copyright (c) 2016-2019 Arkadiusz Bokowy
 *
 * This file is a part of bluez-alsa.
 *
 * This project is licensed under the terms of the MIT license.
 *
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <getopt.h>

#include <check.h>

#include "inc/sine.inc"
#include "../src/at.c"
#include "../src/ba-adapter.c"
#include "../src/ba-device.c"
#include "../src/ba-transport.c"
#include "../src/bluealsa.c"
#include "../src/io.c"
#include "../src/msbc.c"
#include "../src/rfcomm.c"
#include "../src/utils.c"
#include "../src/shared/ffb.c"
#include "../src/shared/log.c"
#include "../src/shared/rt.c"

int bluealsa_dbus_transport_register(struct ba_transport *t, GError **error) {
	debug("%s: %p", __func__, (void *)t); (void)error;
	return 0; }
void bluealsa_dbus_transport_update(struct ba_transport *t, unsigned int mask) {
	debug("%s: %p %#x", __func__, (void *)t, mask); }
void bluealsa_dbus_transport_unregister(struct ba_transport *t) {
	debug("%s: %p", __func__, (void *)t); }

static const a2dp_sbc_t config_sbc_44100_stereo = {
	.frequency = SBC_SAMPLING_FREQ_44100,
	.channel_mode = SBC_CHANNEL_MODE_STEREO,
	.block_length = SBC_BLOCK_LENGTH_16,
	.subbands = SBC_SUBBANDS_8,
	.allocation_method = SBC_ALLOCATION_LOUDNESS,
	.min_bitpool = SBC_MIN_BITPOOL,
	.max_bitpool = SBC_MAX_BITPOOL,
};

static const a2dp_aac_t config_aac_44100_stereo = {
	.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC,
	AAC_INIT_FREQUENCY(AAC_SAMPLING_FREQ_44100)
	.channels = AAC_CHANNELS_2,
	.vbr = 1,
	AAC_INIT_BITRATE(0xFFFF)
};

static const a2dp_aptx_t config_aptx_44100_stereo = {
	.info.vendor_id = APTX_VENDOR_ID,
	.info.codec_id = APTX_CODEC_ID,
	.frequency = APTX_SAMPLING_FREQ_44100,
	.channel_mode = APTX_CHANNEL_MODE_STEREO,
};

static const a2dp_ldac_t config_ldac_44100_stereo = {
	.info.vendor_id = LDAC_VENDOR_ID,
	.info.codec_id = LDAC_CODEC_ID,
	.frequency = LDAC_SAMPLING_FREQ_44100,
	.channel_mode = LDAC_CHANNEL_MODE_STEREO,
};

static struct ba_adapter *adapter = NULL;
static struct ba_device *device1 = NULL;
static struct ba_device *device2 = NULL;
static unsigned int aging = 0;

/**
 * Helper function for timed thread join.
 *
 * This function takes the timeout value in microseconds. */
static int pthread_timedjoin(pthread_t thread, void **retval, useconds_t usec) {

	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += (long)usec * 1000;

	/* normalize timespec structure */
	ts.tv_sec += ts.tv_nsec / (long)1e9;
	ts.tv_nsec = ts.tv_nsec % (long)1e9;

	return pthread_timedjoin_np(thread, retval, &ts);
}

/**
 * BT data generated by the encoder. */
static struct {
	uint8_t data[1024];
	size_t len;
} test_bt_data[10];

static void test_a2dp_encoding(struct ba_transport *t, void *(*cb)(void *)) {

	int bt_fds[2];
	int pcm_fds[2];

	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, bt_fds), 0);
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, pcm_fds), 0);

	t->type.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE;
	t->state = TRANSPORT_ACTIVE;
	t->bt_fd = bt_fds[1];
	t->a2dp.pcm.fd = pcm_fds[1];

	pthread_t thread;
	pthread_create(&thread, NULL, cb, ba_transport_ref(t));

	struct pollfd pfds[] = {{ bt_fds[0], POLLIN, 0 }};
	int16_t buffer[1024 * 10];
	size_t i = 0;

	snd_pcm_sine_s16le(buffer, ARRAYSIZE(buffer), 2, 0, 0.01);
	ck_assert_int_eq(write(pcm_fds[0], buffer, sizeof(buffer)), sizeof(buffer));

	memset(test_bt_data, 0, sizeof(test_bt_data));
	while (poll(pfds, ARRAYSIZE(pfds), 500) > 0) {

		char label[35];
		ssize_t len;

		if ((len = read(bt_fds[0], buffer, t->mtu_write)) <= 0)
			break;

		if (i < ARRAYSIZE(test_bt_data)) {
			memcpy(test_bt_data[i].data, buffer, len);
			test_bt_data[i++].len = len;
		}

		sprintf(label, "BT data [len: %3zd]", len);
		hexdump(label, buffer, len);

	}

	ck_assert_int_eq(pthread_cancel(thread), 0);
	ck_assert_int_eq(pthread_timedjoin(thread, NULL, 1e6), 0);

	close(pcm_fds[0]);
	close(bt_fds[0]);
}

static void test_a2dp_decoding(struct ba_transport *t, void *(*cb)(void *)) {

	int bt_fds[2];
	int pcm_fds[2];

	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, bt_fds), 0);
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pcm_fds), 0);

	t->type.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	t->state = TRANSPORT_ACTIVE;
	t->bt_fd = bt_fds[1];
	t->a2dp.pcm.fd = pcm_fds[1];

	pthread_t thread;
	pthread_create(&thread, NULL, cb, ba_transport_ref(t));

	struct pollfd pfds[] = {{ pcm_fds[0], POLLIN, 0 }};
	int16_t buffer[2048];
	size_t i = 0;

	while (
			i < ARRAYSIZE(test_bt_data) ||
			poll(pfds, ARRAYSIZE(pfds), 500) > 0) {

		if (i < ARRAYSIZE(test_bt_data) && test_bt_data[i].len != 0)
			ck_assert_int_gt(write(bt_fds[0], test_bt_data[i].data, test_bt_data[i].len), 0);
		i++;

		ssize_t len;
		if ((len = read(pfds[0].fd, buffer, sizeof(buffer))) > 0)
			debug("Decoded samples: %zd", len / sizeof(int16_t));

	}

	ck_assert_int_eq(pthread_cancel(thread), 0);
	ck_assert_int_eq(pthread_timedjoin(thread, NULL, 1e6), 0);

	close(pcm_fds[0]);
	close(bt_fds[0]);
}

static void test_a2dp_aging(struct ba_transport *t1, struct ba_transport *t2,
		void *(*enc)(void *), void *(*dec)(void *)) {

	int bt_fds[2];
	int pcm_fds[2];

	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, bt_fds), 0);
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pcm_fds), 0);

	t1->type.profile = BA_TRANSPORT_PROFILE_A2DP_SOURCE;
	t2->type.profile = BA_TRANSPORT_PROFILE_A2DP_SINK;
	t1->state = TRANSPORT_ACTIVE;
	t2->state = TRANSPORT_ACTIVE;
	t1->bt_fd = bt_fds[1];
	t2->bt_fd = bt_fds[0];
	t1->a2dp.pcm.fd = pcm_fds[1];
	t2->a2dp.pcm.fd = pcm_fds[0];

	int16_t buffer[1024 * 10];
	snd_pcm_sine_s16le(buffer, ARRAYSIZE(buffer), 2, 0, 0.01);
	ck_assert_int_eq(write(pcm_fds[0], buffer, sizeof(buffer)), sizeof(buffer));

	pthread_t thread1;
	pthread_t thread2;
	pthread_create(&thread1, NULL, enc, ba_transport_ref(t1));
	pthread_create(&thread2, NULL, dec, ba_transport_ref(t2));

	sleep(aging);

	ck_assert_int_eq(pthread_cancel(thread1), 0);
	ck_assert_int_eq(pthread_cancel(thread2), 0);

	ck_assert_int_eq(pthread_timedjoin(thread1, NULL, 1e6), 0);
	ck_assert_int_eq(pthread_timedjoin(thread2, NULL, 1e6), 0);

}

static void test_sco(struct ba_transport *t, void *(*cb)(void *)) {

	int sco_fds[2];
	int pcm_mic_fds[2];
	int pcm_spk_fds[2];

	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sco_fds), 0);
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, pcm_mic_fds), 0);
	ck_assert_int_eq(socketpair(AF_UNIX, SOCK_STREAM, 0, pcm_spk_fds), 0);

	t->state = TRANSPORT_ACTIVE;
	t->bt_fd = sco_fds[1];
	t->sco.mic_pcm.fd = pcm_mic_fds[1];
	t->sco.spk_pcm.fd = pcm_spk_fds[1];

	pthread_t thread;
	pthread_create(&thread, NULL, cb, ba_transport_ref(t));

	struct pollfd pfds[] = {
		{ sco_fds[0], POLLIN, 0 },
		{ pcm_mic_fds[0], POLLIN, 0 }};
	int16_t buffer[1024 * 4];
	size_t i = 0;

	snd_pcm_sine_s16le(buffer, ARRAYSIZE(buffer), 2, 0, 0.01);
	ck_assert_int_eq(write(pcm_spk_fds[0], buffer, sizeof(buffer)), sizeof(buffer));

	memset(test_bt_data, 0, sizeof(test_bt_data));
	while (poll(pfds, ARRAYSIZE(pfds), 500) > 0) {

		if (pfds[0].revents & POLLIN) {

			char label[35];
			ssize_t len;

			if ((len = read(sco_fds[0], buffer, t->mtu_write)) <= 0)
				break;

			sprintf(label, "BT data [len: %3zd]", len);
			hexdump(label, buffer, len);

			if (i < ARRAYSIZE(test_bt_data)) {
				memcpy(test_bt_data[i].data, buffer, len);
				test_bt_data[i++].len = len;
			}

			ck_assert_int_gt(write(sco_fds[0], buffer, len), 0);

		}

		if (pfds[1].revents & POLLIN) {
			ck_assert_int_gt(read(pcm_mic_fds[0], buffer, sizeof(buffer)), 0);
		}

	}

	ck_assert_int_eq(pthread_cancel(thread), 0);
	ck_assert_int_eq(pthread_timedjoin(thread, NULL, 1e6), 0);

	close(pcm_spk_fds[0]);
	close(pcm_mic_fds[0]);
	close(sco_fds[0]);
}

static int test_transport_acquire(struct ba_transport *t) {
	debug("Acquire transport: %d", t->bt_fd);
	return 0;
}

static int test_transport_release_bt_a2dp(struct ba_transport *t) {
	free(t->bluez_dbus_owner); t->bluez_dbus_owner = NULL;
	return transport_release_bt_a2dp(t);
}

START_TEST(test_a2dp_sbc) {

	struct ba_transport_type ttype = { .codec = A2DP_CODEC_SBC };
	struct ba_transport *t = ba_transport_new_a2dp(device1, ttype, ":test", "/path/sbc",
			&config_sbc_44100_stereo, sizeof(config_sbc_44100_stereo));

	t->acquire = test_transport_acquire;
	t->release = test_transport_release_bt_a2dp;

	t->mtu_write = 153 * 3;
	test_a2dp_encoding(t, io_thread_a2dp_source_sbc);

	t->mtu_read = t->mtu_write;
	test_a2dp_decoding(t, io_thread_a2dp_sink_sbc);

} END_TEST

START_TEST(test_a2dp_aging_sbc) {

	struct ba_transport_type ttype = { .codec = A2DP_CODEC_SBC };
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/sbc",
			&config_sbc_44100_stereo, sizeof(config_sbc_44100_stereo));
	struct ba_transport *t2 = ba_transport_new_a2dp(device2, ttype, ":test", "/path/sbc",
			&config_sbc_44100_stereo, sizeof(config_sbc_44100_stereo));

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	t1->mtu_write = t2->mtu_read = 153 * 3;
	test_a2dp_aging(t1, t2, io_thread_a2dp_source_sbc, io_thread_a2dp_sink_sbc);

} END_TEST

#if ENABLE_AAC
START_TEST(test_a2dp_aac) {

	struct ba_transport_type ttype = { .codec = A2DP_CODEC_MPEG24 };
	struct ba_transport *t = ba_transport_new_a2dp(device1, ttype, ":test", "/path/aac",
			&config_aac_44100_stereo, sizeof(config_aac_44100_stereo));

	t->acquire = test_transport_acquire;
	t->release = test_transport_release_bt_a2dp;

	t->mtu_write = 64;
	test_a2dp_encoding(t, io_thread_a2dp_source_aac);

	t->mtu_read = t->mtu_write;
	test_a2dp_decoding(t, io_thread_a2dp_sink_aac);

} END_TEST
#endif

#if ENABLE_AAC
START_TEST(test_a2dp_aging_aac) {

	struct ba_transport_type ttype = { .codec = A2DP_CODEC_MPEG24 };
	struct ba_transport *t1 = ba_transport_new_a2dp(device1, ttype, ":test", "/path/aac",
			&config_aac_44100_stereo, sizeof(config_aac_44100_stereo));
	struct ba_transport *t2 = ba_transport_new_a2dp(device2, ttype, ":test", "/path/aac",
			&config_aac_44100_stereo, sizeof(config_aac_44100_stereo));

	t1->acquire = t2->acquire = test_transport_acquire;
	t1->release = t2->release = test_transport_release_bt_a2dp;

	t1->mtu_write = t2->mtu_read = 450;
	test_a2dp_aging(t1, t2, io_thread_a2dp_source_aac, io_thread_a2dp_sink_aac);

} END_TEST
#endif

#if ENABLE_APTX
START_TEST(test_a2dp_aptx) {

	struct ba_transport_type ttype = { .codec = A2DP_CODEC_VENDOR_APTX };
	struct ba_transport *t = ba_transport_new_a2dp(device1, ttype, ":test", "/path/aptx",
			&config_aptx_44100_stereo, sizeof(config_aptx_44100_stereo));

	t->acquire = test_transport_acquire;
	t->release = test_transport_release_bt_a2dp;

	t->mtu_write = 40;
	test_a2dp_encoding(t, io_thread_a2dp_source_aptx);

} END_TEST
#endif

#if ENABLE_LDAC
START_TEST(test_a2dp_ldac) {

	struct ba_transport_type ttype = { .codec = A2DP_CODEC_VENDOR_LDAC };
	struct ba_transport *t = ba_transport_new_a2dp(device1, ttype, ":test", "/path/ldac",
			&config_ldac_44100_stereo, sizeof(config_ldac_44100_stereo));

	t->acquire = test_transport_acquire;
	t->release = test_transport_release_bt_a2dp;

	t->mtu_write = RTP_HEADER_LEN + sizeof(rtp_media_header_t) + 679;
	test_a2dp_encoding(t, io_thread_a2dp_source_ldac);

} END_TEST
#endif

START_TEST(test_sco_cvsd) {

	struct ba_transport_type ttype = { .profile = BA_TRANSPORT_PROFILE_HSP_AG };
	struct ba_transport *t = ba_transport_new_sco(device1, ttype, ":test", "/path/sco/cvsd", NULL);

	t->mtu_read = t->mtu_write = 48;
	t->acquire = test_transport_acquire;

	ba_transport_send_signal(t, TRANSPORT_PING);
	test_sco(t, io_thread_sco);

} END_TEST

#if ENABLE_MSBC
START_TEST(test_sco_msbc) {

	struct ba_transport_type ttype = {
		.profile = BA_TRANSPORT_PROFILE_HFP_AG,
		.codec = HFP_CODEC_MSBC };
	struct ba_transport *t = ba_transport_new_sco(device1, ttype, ":test", "/path/sco/msbc", NULL);

	t->mtu_read = t->mtu_write = 24;
	t->acquire = test_transport_acquire;

	ba_transport_send_signal(t, TRANSPORT_PING);
	test_sco(t, io_thread_sco);

} END_TEST
#endif

int main(int argc, char *argv[]) {

	int opt;
	const char *opts = "h";
	struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ "aging", required_argument, NULL, 'a' },
		{ 0, 0, 0, 0 },
	};

	struct {
		const char *name;
		unsigned int flag;
	} codecs[] = {
#define TEST_CODEC_SBC  (1 << 0)
		{ "SBC", TEST_CODEC_SBC },
#define TEST_CODEC_AAC  (1 << 1)
		{ "AAC", TEST_CODEC_AAC },
#define TEST_CODEC_APTX (1 << 2)
		{ "APTX", TEST_CODEC_APTX },
#define TEST_CODEC_LDAC (1 << 3)
		{ "LDAC", TEST_CODEC_LDAC },
#define TEST_CODEC_CVSD (1 << 4)
		{ "CVSD", TEST_CODEC_CVSD },
#define TEST_CODEC_MSBC (1 << 5)
		{ "mSBC", TEST_CODEC_MSBC },
	};

	while ((opt = getopt_long(argc, argv, opts, longopts, NULL)) != -1)
		switch (opt) {
		case 'h' /* --help */ :
			printf("usage: %s [--aging=SEC] [codec ...]\n", argv[0]);
			return 0;
		case 'a' /* --aging=SEC */ :
			aging = atoi(optarg);
			break;
		default:
			fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
			return 1;
		}

	unsigned int enabled_codecs = 0xFFFF;
	size_t i;

	if (optind != argc)
		enabled_codecs = 0;
	for (; optind < argc; optind++)
		for (i = 0; i < ARRAYSIZE(codecs); i++)
			if (strcasecmp(argv[optind], codecs[i].name) == 0)
				enabled_codecs |= codecs[i].flag;

	bdaddr_t addr1 = {{ 1, 2, 3, 4, 5, 6 }};
	bdaddr_t addr2 = {{ 1, 2, 3, 7, 8, 9 }};
	adapter = ba_adapter_new(0);
	device1 = ba_device_new(adapter, &addr1);
	device2 = ba_device_new(adapter, &addr2);

	Suite *s = suite_create(__FILE__);
	TCase *tc = tcase_create(__FILE__);
	SRunner *sr = srunner_create(s);

	suite_add_tcase(s, tc);
	tcase_set_timeout(tc, aging + 5);

	if (enabled_codecs & TEST_CODEC_SBC)
		tcase_add_test(tc, test_a2dp_sbc);
#if ENABLE_AAC
	config.aac_afterburner = true;
	if (enabled_codecs & TEST_CODEC_AAC)
		tcase_add_test(tc, test_a2dp_aac);
#endif
#if ENABLE_APTX
	if (enabled_codecs & TEST_CODEC_APTX)
		tcase_add_test(tc, test_a2dp_aptx);
#endif
#if ENABLE_LDAC
	config.ldac_abr = true;
	config.ldac_eqmid = LDACBT_EQMID_HQ;
	if (enabled_codecs & TEST_CODEC_LDAC)
		tcase_add_test(tc, test_a2dp_ldac);
#endif
	if (enabled_codecs & TEST_CODEC_CVSD)
		tcase_add_test(tc, test_sco_cvsd);
#if ENABLE_MSBC
	if (enabled_codecs & TEST_CODEC_MSBC)
		tcase_add_test(tc, test_sco_msbc);
#endif

	if (aging > 0) {
		if (enabled_codecs & TEST_CODEC_SBC)
			tcase_add_test(tc, test_a2dp_aging_sbc);
#if ENABLE_AAC
		if (enabled_codecs & TEST_CODEC_AAC)
			tcase_add_test(tc, test_a2dp_aging_aac);
#endif
	}

	srunner_run_all(sr, CK_ENV);
	int nf = srunner_ntests_failed(sr);
	srunner_free(sr);

	return nf == 0 ? 0 : 1;
}
