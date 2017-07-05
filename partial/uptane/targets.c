#include "targets.h"
#include <string.h>
#include <limits.h>

#include "uptane_time.h"
#include "uptane_config.h"
#include "crypto.h"
/* Grammar of director/targets.json
 *
 
   {"signatures":\[({"keyid":"<hexstring>","method":"<string>","sig":"<hexstring>"},?)+\],"signed":{"_type":"<string>","expires":"<time>","targets":{("<string>":{"custom":{"ecu_identifier":"<string>","hardware_identifier":"<string>","release_counter":<number>},"hashes":{("<string>":"<hexstring>")+},"length":<number>},?)+},"version":<number>}}
 
*/

#define MAXFIXED 31 /* Longest fixed string in targets.json is":{\"custom\":{\"ecu_identifier\":"*/

struct targets_ctx {
	/* Read callback and its argument */
	targets_read_t read;
	targets_peek_t peek;
	void* priv;

	/* Inputs */
	uint32_t version_prev;
	uptane_time_t time;
	crypto_key_and_signature_t* sigs[CONFIG_UPTANE_TARGETS_MAX_SIGS]; 
	uint32_t num_keys;
	uint32_t threshold;
	const uint8_t* ecu_id;
	const uint8_t* hardware_id;

	/* Outputs */
	uint8_t sha512_hash[SHA512_HASH_SIZE]; /* Only one hash is currently supported*/
	uint32_t version;
	uint32_t length;
	targets_result_t res;

	/* Intermediate state */
	bool in_signed;
	bool sig_valid[CONFIG_UPTANE_TARGETS_MAX_SIGS];
	crypto_verify_ctx_t* sig_ctx[CONFIG_UPTANE_TARGETS_MAX_SIGS];

};

#ifdef CONFIG_UPTANE_NOMALLOC
#  if (CONFIG_UPTANE_TARGETS_NUM_CONTEXTS == 1)
#define SINGLECONTEXT
static targets_ctx_t targets_ctx_s;
#  else
static targets_ctx_t targets_ctxs[CONFIG_UPTANE_TARGETS_NUM_CONTEXTS];
static bool targets_ctx_busy[CONFIG_UPTANE_TARGETS_NUM_CONTEXTS] = {0, }; /*could be reduce to a bitmask when new/free performance is an issue*/
#  endif
#endif

targets_ctx_t* targets_ctx_new() {
#if defined(SINGLECONTEXT)
	return &targets_ctx_s;
#elif defined(CONFIG_UPTANE_NOMALLOC)
	int i;

	for(i = 0; i < CONFIG_UPTANE_TARGETS_NUM_CONTEXTS; i++)
		if(!targets_ctx_busy[i])
			return &targets_ctxs[i];
	return NULL;
#else
	return malloc(sizeof(targets_ctx_t));

#endif
}

void targets_ctx_free(targets_ctx_t* ctx) {
	int i;
#if defined(CONFIG_UPTANE_NOMALLOC) && !defined(SINGLECONTEXT)
	uintptr_t j = ((uintptr_t) ctx - (uintptr_t) targets_ctxs)/sizeof(targets_ctx_t); 
#endif

	for(i = 0; i < ctx->num_keys; i++) {
		crypto_sig_free(ctx->sigs[i]);
		if(ctx->sig_valid[i]) 
			crypto_verify_ctx_free(ctx->sig_ctx[i]);
	}

#if defined(SINGLECONTEXT)
#elif defined(CONFIG_UPTANE_NOMALLOC)
	targets_ctx_busy[j] = 0;
#else
	free(ctx);
#endif
}

bool targets_init(targets_ctx_t* ctx, int version_prev, uptane_time_t time,
		  const uint8_t* ecu_id, const uint8_t* hardware_id,
		  const crypto_key_t* keys, int key_num, int threshold,
		  targets_read_t read_cb, targets_peek_t peek_cb, void* cb_priv) {
	int i;

	ctx->version_prev = version_prev;
	ctx->time = time;
	for (i = 0; i < key_num; i++) {
		if(i >= CONFIG_UPTANE_TARGETS_MAX_SIGS)
			break;
		ctx->sigs[i] = crypto_sig_new();
		if(!ctx->sigs[i])
			return false;
		ctx->sigs[i]->key = &keys[i];
		ctx->sig_valid[i] = false;
	}
	ctx->num_keys = i;

	ctx->threshold = threshold;
	ctx->ecu_id = ecu_id;
	ctx->hardware_id = hardware_id;

	ctx->read = read_cb;
	ctx->peek = peek_cb;
	ctx->priv = cb_priv;
	ctx->in_signed = false;
	return true;
}

#if defined(SINGLECONTEXT)
#define read_verify_wrapper(ctx, buf, len) read_verify_wrapper_s(buf, len)
static void read_verify_wrapper_s(uint8_t* buf, int len) {
	targets_ctx_t* ctx = &targets_ctx_s;
#else
static void read_verify_wrapper(targets_ctx_t* ctx, uint8_t* buf, int len) {
#endif /*defined(SINGLECONTEXT)*/
	ctx->read(ctx->priv, buf, len);

	if(ctx->in_signed) {
		int i;
		for(i = 0; i < ctx->num_keys; i++)
			if(ctx->sig_valid[i])
				crypto_verify_feed(ctx->sig_ctx[i], buf, len);
	}
}

#if defined(SINGLECONTEXT)
#define peek_wrapper(ctx, buf) peek_wrapper_s(buf)
static void peek_wrapper_s(uint8_t* buf) {
	targets_ctx_t* ctx = &targets_ctx_s;
#else
static void peek_wrapper(targets_ctx_t* ctx, uint8_t* buf) {
#endif /*defined(SINGLECONTEXT)*/
	ctx->peek(ctx->priv, buf);
}
static bool is_hex(uint8_t c) {
	return (c >= '0' && c <= '9') ||
	       (c >= 'A' && c <= 'F') ||
	       (c >= 'a' && c <= 'f');
}
static uint8_t from_hex(uint8_t hi, uint8_t lo) {
	uint8_t res;

	if(hi >= '0' && hi <= '9')
		res = hi - '0';
	else if (hi >= 'A' && hi <= 'F')
		res = hi - 'A' + 10;
	else /* inputs are validated outside */
		res = hi - 'a' + 10;

	res <<= 4;

	if(lo >= '0' && lo <= '9')
		res |= lo - '0';
	else if (lo >= 'A' && lo <= 'F')
		res |= lo - 'A' + 10;
	else /* inputs are validated outside */
		res |= lo - 'a' + 10;

	return res;
}
/* TODO: consider having more return codes to differentiate between read errors
   and malformed JSON*/
#if defined(SINGLECONTEXT)
#define one_char(ctx, buf) one_char_s(buf)
static inline void one_char_s(uint8_t* buf) {
#else
static inline void one_char(targets_ctx_t* ctx, uint8_t* buf) {
#endif
	read_verify_wrapper(ctx, buf, 1);
}

#if defined(SINGLECONTEXT)
#define skip_bytes(ctx, buf) skip_bytes_s(buf)
static void skip_bytes_s(int n) {
#else
static void skip_bytes(targets_ctx_t* ctx, int n) {
#endif /*defined(SINGLECONTEXT)*/
	uint8_t b;
	while(--n)
		one_char(ctx, &b);
}
#define fixed_data(ctx, str) skip_bytes(ctx, sizeof(str))

/* Hex string including quotes*/
#if defined(SINGLECONTEXT)
#define hex_string(ctx, data, max_len) hex_string_s(data, max_len)
static int hex_string_s(uint8_t* data, int max_len) {
#else
static int hex_string(targets_ctx_t* ctx, uint8_t* data, int max_len) {
#endif /*defined(SINGLECONTEXT)*/
	int i;
	uint8_t hex[2];

	one_char(ctx, &hex[0]);

	if(hex[0] != '\"')
		return -1;

	for(i = 0; i < (max_len << 1) + 1; i++) {
		int ind = i&1;
		one_char(ctx, &hex[ind]);

		if(ind) {
			if(!is_hex(hex[1]))
				return -1;
			data[i>>1] = from_hex(hex[0], hex[1]);
		} else {
			if(hex[0] == '\"')
				return (i >> 1);
			if(!is_hex(hex[0]))
				return -1;
		}
	}

	/* didn't encounter quotation mark */
	return -1;
}

/* String including quotes*/
#if defined(SINGLECONTEXT)
#define text_string(ctx, data, max_len) text_string_s(data, max_len)
static bool text_string_s(uint8_t* data, int max_len) {
#else
static bool text_string(targets_ctx_t* ctx, uint8_t* data, int max_len) {
#endif /*defined(SINGLECONTEXT)*/
	uint8_t byte;
	int i;

	one_char(ctx, &byte);

	if(byte != '\"')
		return false;

	for(i = 0; i < max_len; i++) {
		one_char(ctx, &byte);
		if(byte == '\"') {
			if(data)
				data[i] = 0;
			return true;
		}
		else {
			if(data)
				data[i] = byte;
		}
	}

	/* Normal return due to end of string didn't happen, string is too long*/
	return false;
}

#if defined(SINGLECONTEXT)
#define ignore_string(ctx) ignore_string_s()
static inline bool ignore_string_s() {
#else
static inline bool ignore_string(targets_ctx_t* ctx) {
#endif /*defined(SINGLECONTEXT)*/
	return text_string(ctx, 0, INT_MAX);
}

#if defined(SINGLECONTEXT)
#define integer_number(ctx, num) integer_number_s(num)
static bool integer_number_s(uint32_t* num) {
#else
static bool integer_number(targets_ctx_t* ctx, uint32_t* num) {
#endif /*defined(SINGLECONTEXT)*/
	uint32_t res = 0;
	bool valid = false;
	uint8_t byte;

	for(;;){
		peek_wrapper(ctx, &byte);
		if(byte >= '0' && byte <= '9') {
			res = res*10 + (byte - '0');
			valid = true;
		}
		else {
			break;
		}
		one_char(ctx, &byte);
	}
	*num = res;
	return valid;
}

/* Expected format is yyyy-mm-ddThh:mm:ssZ*/
#if defined(SINGLECONTEXT)
#define time_string(ctx, time) time_string_s(time)
static bool time_string_s(uptane_time_t* time) {
#else
static bool time_string(targets_ctx_t *ctx, uptane_time_t* time) {
#endif /*defined(SINGLECONTEXT)*/
	uint32_t num;

	fixed_data(ctx, "\"");
	if(!integer_number(ctx, &num))
		return false;
	time->year = num;

	fixed_data(ctx, "-");
	if(!integer_number(ctx, &num))
		return false;
	time->month = num;

	fixed_data(ctx, "-");
	if(!integer_number(ctx, &num))
		return false;
	time->day = num;

	fixed_data(ctx, "T");
	if(!integer_number(ctx, &num))
		return false;
	time->hour = num;

	fixed_data(ctx, ":");
	if(!integer_number(ctx, &num))
		return false;
	time->minute = num;

	fixed_data(ctx, ":");
	if(!integer_number(ctx, &num))
		return false;
	time->second = num;

	fixed_data(ctx, "Z\"");
	return true;
}

targets_result_t targets_process(targets_ctx_t* ctx) {
	int i;
	int j;
	uint8_t buf[CONFIG_UPTANE_TARGETS_BUF_SIZE];
	int valid_sigs = 0;
	uptane_time_t time;
	uint32_t number = 0;
	bool got_image = false;
	bool got_hash = false;

	fixed_data(ctx, "{\"signatures\":[");

	for(i = 0; i < CONFIG_UPTANE_TARGETS_MAX_SIGS; i++) {
		bool ignore_sig = true;
		int current_sig;
		fixed_data(ctx, "{\"keyid\":");
		if(hex_string(ctx, buf, CRYPTO_KEYID_LEN) != CRYPTO_KEYID_LEN)
			return TARGETS_JSONERR;

		/* Find matching key */
		for(j = 0; j < ctx->num_keys; j++)
			if(!memcmp(ctx->sigs[i]->key->keyid, buf, CRYPTO_KEYID_LEN)) {
				ignore_sig = false;
				current_sig = j;
				break;
			}
		fixed_data(ctx, ",\"method\":");
		
		if(!text_string(ctx, buf, CONFIG_UPTANE_TARGETS_BUF_SIZE))
			return TARGETS_JSONERR;

		if(!crypto_keytype_supported((const char*) buf))
			ignore_sig = true;

		fixed_data(ctx, ",\"sig\":");
		if(ignore_sig) {
			if(!ignore_string(ctx))
				return TARGETS_JSONERR;
		} else {
			/* In theory we can support multiple kinds of signatures,
			 * let CRYPTO_SIGNATURE_LEN be the maximum */
			if(hex_string(ctx, ctx->sigs[current_sig]->sig, CRYPTO_SIGNATURE_LEN) <= 0)
				return TARGETS_JSONERR;
			ctx->sig_valid[current_sig] = true;
			ctx->sig_ctx[current_sig] = crypto_verify_ctx_new();
			if(!ctx->sig_ctx[current_sig])
				return TARGETS_NOMEM;
			crypto_verify_init(ctx->sig_ctx[current_sig], ctx->sigs[current_sig]);
		}

		fixed_data(ctx, "}");

		one_char(ctx, buf);

		/* End of signature array*/
		if(*buf == ']')
			break;
		/* If array hasn't ended, expecting new element after a comma */
		else if(*buf != ',')
			return TARGETS_JSONERR;
	}

	/* Didn't exit due to end of array, too many signatures in targets.json*/
	if(i == CONFIG_UPTANE_TARGETS_MAX_SIGS)
		return TARGETS_JSONERR;

	fixed_data(ctx, ",\"signed\":");

	/* signed section started, verification is performed in read_verify_wrapper */
	ctx->in_signed = true;
	fixed_data(ctx, "{\"_type\":");

	if(!text_string(ctx, buf, CONFIG_UPTANE_TARGETS_BUF_SIZE))
		return TARGETS_JSONERR;

	if(strcmp((const char*) buf, "Targets"))
		return TARGETS_WRONGTYPE;

	fixed_data(ctx, ",\"expires\":");

	if(!time_string(ctx, &time))
		return TARGETS_JSONERR;

	if(uptane_time_greater(ctx->time, time))
		return TARGETS_EXPIRED;

	fixed_data(ctx, ",\"targets\":{");

	/* Iterate over targets */
	for(;;) {
		bool ignore_image = false;

		/* Target path */
		if(!ignore_string(ctx))
			return TARGETS_JSONERR;

		fixed_data(ctx, ":{\"custom\":{\"ecu_identifier\":");

		if(!text_string(ctx, buf, CONFIG_UPTANE_TARGETS_BUF_SIZE))
			return TARGETS_JSONERR;

		if(strcmp((const char*) buf, (const char*) ctx->ecu_id))
			ignore_image = true;

		fixed_data(ctx, ",\"hardware_identifier\":");

		if(!text_string(ctx, buf, CONFIG_UPTANE_TARGETS_BUF_SIZE))
			return TARGETS_JSONERR;

		if(strcmp((const char*) buf, (const char*) ctx->hardware_id))
			ignore_image = true;

		fixed_data(ctx, ",\"release_counter\":");

		/* Ignore release counter */
		if(!integer_number(ctx, &number))
			return TARGETS_JSONERR;

		fixed_data(ctx, "},\"hashes\":{");

		/* Iterate over hashes */
		for(;;) {
			if(!text_string(ctx, buf, CONFIG_UPTANE_TARGETS_BUF_SIZE))
				return TARGETS_JSONERR;
			fixed_data(ctx, ":");

			if(!ignore_image && !strcmp((const char*) buf, "sha512")) {
				if(hex_string(ctx, ctx->sha512_hash,
					      SHA512_HASH_SIZE) != SHA512_HASH_SIZE)
					return TARGETS_JSONERR;
				got_hash = true;
			} else {
				if(!ignore_string(ctx))
					return TARGETS_JSONERR;
			}

			one_char(ctx, buf);

			if(*buf == '}')
				break;
			else if(*buf != ',')
				return TARGETS_JSONERR;
		} /* iterate over hashes */

		fixed_data(ctx, ",\"length\":");

		if(!integer_number(ctx, &ctx->length))
			return TARGETS_JSONERR;

		if(!ignore_image) {
			if(got_image)
				return TARGETS_ECUDUPLICATE;
			else
				got_image = true;
		}
		fixed_data(ctx, "}");

		one_char(ctx, buf);
		if(*buf == '}')
			break;
		else if(*buf != ',')
			return TARGETS_JSONERR;

	} /* iterate over targets*/

	fixed_data(ctx, ",\"version\":");

	if(!integer_number(ctx, &ctx->version))
		return TARGETS_JSONERR;

	if(ctx->version < ctx->version_prev)
		return TARGETS_DOWNGRADE;

	fixed_data(ctx, "}");

	/* signed section ended, verify */
	ctx->in_signed = false;
	for(i = 0; i < ctx->num_keys; i++)
		if(ctx->sig_valid[i] && crypto_verify_result(ctx->sig_ctx[i]))
			++valid_sigs;
	if(valid_sigs < ctx->threshold)
		return TARGETS_SIGFAIL;

	/* trailing '}', EOF */
	fixed_data(ctx, "}");

	if(!got_image)
		return TARGETS_OK_NOIMAGE;

	if(!got_hash)
		return TARGETS_NOHASH;

	if(ctx->version == ctx->version_prev)
		return TARGETS_OK_NOUPDATE;

	return TARGETS_OK_UPDATE;
}