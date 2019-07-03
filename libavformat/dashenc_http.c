#include "dashenc_http.h"

#include "config.h"
#include "libavutil/avutil.h"
#include <pthread.h>

#include "avio_internal.h"
#include "utils.c"
#include "dashenc_pool.h"
#if CONFIG_HTTP_PROTOCOL
#include "http.h"
#endif

//TODO don't hardcode the max number of threads
#define nr_of_threads 20

typedef struct _thread_data_t {
    int tid;
    //pthread_t thread;
    AVIOContext *out;
    int claimed; /* This thread is claimed for a specific request */
    int opened;  /* out is opened */
    int64_t release_time;

    //Request specific data
    int must_succeed; /* If 1 the request must succeed, otherwise we'll crash the program */
} thread_data_t;

static thread_data_t thr_data[nr_of_threads];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *thread_pool;


/**
 * Claims a free connection and returns the connection number.
 * Released connections are used first.
 */
static int claim_connection(char *url) {
    int64_t lowest_release_time = av_gettime() / 1000;
    int data_nr = -1;
    pthread_mutex_lock(&lock);

    for(int i = 0; i < nr_of_threads; i++) {
        thread_data_t *data = &thr_data[i];
        if (!data->claimed) {
            if ((data_nr == -1) || (data->release_time != 0 && data->release_time < lowest_release_time)) {
                data_nr = i;
                lowest_release_time = data->release_time;
            }
        }
    }

    if (data_nr == -1) {
        av_log(NULL, AV_LOG_ERROR, "Could not claim connection for url: %s\n", url);
        pthread_mutex_unlock(&lock);
        return -1;
    }

    av_log(NULL, AV_LOG_INFO, "Claimed conn_id: %d, url: %s\n", data_nr, url);
    thr_data[data_nr].claimed = 1;
    thr_data[data_nr].tid = data_nr;
    pthread_mutex_unlock(&lock);
    return data_nr;
}

/**
 * Opens a request on a free connection and returns the connection number
 */
static int open_request(AVFormatContext *s, char *url, AVDictionary **options) {
    int ret;
    int conn_nr = claim_connection(url);
    thread_data_t *data = &thr_data[conn_nr];

    if (data->opened)
        av_log(s, AV_LOG_WARNING, "open_request while connection might be open. This is TODO for when not using persistent connections. conn_nr: %d\n", conn_nr);

    ret = s->io_open(s, &data->out, url, AVIO_FLAG_WRITE, options);
    if (ret >= 0) {
        ret = conn_nr;
        data->opened = 1;
    }

    return ret;
}

static void force_release_connection(thread_data_t *data) {
    pthread_mutex_lock(&lock);
    data->opened = 0;
    data->claimed = 0;
    pthread_mutex_unlock(&lock);
}

static void abort_if_needed(int mustSucceed) {
    if (mustSucceed) {
        av_log(NULL, AV_LOG_ERROR, "Abort because request needs to succeed and it did not.\n");
        abort();
    }
}

/**
 * Claim a connection and start a new request.
 * The claimed connection number is returned.
 */
int pool_io_open(AVFormatContext *s, char *filename,
                 AVDictionary **options, int http_persistent, int must_succeed) {

    int http_base_proto = filename ? ff_is_http_proto(filename) : 0;
    int ret = AVERROR_MUXER_NOT_FOUND;

    if (!http_base_proto || !http_persistent) {
        //open_request returns the newly claimed conn_nr
        ret = open_request(s, filename, options);
        av_log(s, AV_LOG_WARNING, "Non HTTP request %s\n", filename);
#if CONFIG_HTTP_PROTOCOL
    } else {
        thread_data_t *data;
        URLContext *http_url_context;
        //claim new item from pool and open connection if needed
        int conn_nr = claim_connection(filename);
        //Crash when we cannot claim a new connection. We should restart ffmpeg in this case.
        av_assert0(conn_nr >= 0);

        data = &thr_data[conn_nr];
        data->must_succeed = must_succeed;
        if (!data->opened) {
            ret = s->io_open(s, &data->out, filename, AVIO_FLAG_WRITE, options);
            if (ret < 0) {
                av_log(s, AV_LOG_WARNING, "Could not open %s\n", filename);
                force_release_connection(data);
                abort_if_needed(must_succeed);
                return ret;
            }

            pthread_mutex_lock(&lock);
            data->opened = 1;
            pthread_mutex_unlock(&lock);
            return conn_nr;
        }

        http_url_context = ffio_geturlcontext(data->out);
        av_assert0(http_url_context);

        ret = ff_http_do_new_request(http_url_context, filename);
        if (ret < 0) {
            int64_t curr_time_ms = av_gettime() / 1000;
            int64_t idle_tims_ms = curr_time_ms - data->release_time;
            av_log(s, AV_LOG_WARNING, "pool_io_open error conn_nr: %d, idle_time: %"PRId64", error: %d, name: %s\n", conn_nr, idle_tims_ms, ret, filename);
            ff_format_io_close(s, &data->out);
            force_release_connection(data);
            abort_if_needed(must_succeed);
            return ret;
        }
        ret = conn_nr;
#endif
    }

    return ret;
}

/**
 * This method closes the request and reads the response.
 * It is supposed to be passed to pthread_create.
 */
static void *thr_io_close(void *arg) {
    thread_data_t *data = (thread_data_t *)arg;
    int ret;
    int64_t release_time = av_gettime() / 1000;

    URLContext *http_url_context = ffio_geturlcontext(data->out);
    av_assert0(http_url_context);
    avio_flush(data->out);

    av_log(NULL, AV_LOG_DEBUG, "thr_io_close thread: %d, addr: %p \n", data->tid, data->out);

    ret = ffurl_shutdown(http_url_context, AVIO_FLAG_WRITE);
    pthread_mutex_lock(&lock);
    if (ret < 0) {
        //TODO: do we need to do some cleanup if ffurl_shutdown fails?
        av_log(NULL, AV_LOG_INFO, "-event- request failed ret=%d, conn_nr: %d, url: %s.\n", ret, data->tid, ff_http_get_url(http_url_context));
        abort_if_needed(data->must_succeed);
        data->opened = 0;
    }

    data->claimed = 0;
    data->release_time = release_time;
    pthread_mutex_unlock(&lock);

    return NULL;
}

/**
 * Closes the request.
 */
void pool_io_close(AVFormatContext *s, char *filename, int conn_nr) {
    thread_data_t *data;

    if (conn_nr < 0) {
        av_log(s, AV_LOG_WARNING, "Invalid conn_nr (pool_io_close) for filename: %s\n", filename);
        return;
    }

    data = &thr_data[conn_nr];
    av_log(NULL, AV_LOG_DEBUG, "pool_io_close conn_nr: %d\n", conn_nr);

    if (!data->opened) {
        av_log(s, AV_LOG_INFO, "Skip closing HTTP request because connection is not opened. Filename: %s\n", filename);
        abort_if_needed(data->must_succeed);
        return;
    }

    pool_enqueue(thread_pool, &thr_data[conn_nr], 0);
}

void pool_free(AVFormatContext *s, int conn_nr) {
    thread_data_t *data;

    if (conn_nr < 0) {
        av_log(s, AV_LOG_WARNING, "Invalid conn_nr (pool_free)\n");
        return;
    }

    data = &thr_data[conn_nr];
    av_log(NULL, AV_LOG_DEBUG, "pool_free conn_nr: %d\n", conn_nr);

    ff_format_io_close(s, &data->out);
    force_release_connection(data);
}

void pool_free_all(AVFormatContext *s) {
    thread_data_t *data;

    av_log(NULL, AV_LOG_DEBUG, "pool_free_all\n");
    for(int i = 0; i < nr_of_threads; i++) {
        data = &thr_data[i];
        if (data->out)
            pool_free(s, i);
    }
}

void pool_write_flush(const unsigned char *buf, int size, int conn_nr) {
    thread_data_t *data;

    if (conn_nr < 0) {
        av_log(NULL, AV_LOG_WARNING, "Invalid conn_nr (pool_write_flush)\n");
        return;
    }

    data = &thr_data[conn_nr];

    avio_write(data->out, buf, size);
    avio_flush(data->out);
}

int pool_avio_write(const unsigned char *buf, int size, int conn_nr) {
    thread_data_t *data;

    if (conn_nr < 0) {
        av_log(NULL, AV_LOG_WARNING, "Invalid conn_nr (pool_avio_write)\n");
        return -1;
    }
    data = &thr_data[conn_nr];

    if (data->out)
        avio_write(data->out, buf, size);

    return 0;
}

/**
 * Set out to the AVIOContext of the given conn_nr
 */
void pool_get_context(AVIOContext **out, int conn_nr) {
    //TODO we probably don't want to expose the AVIOContext
    thread_data_t *data;

    if (conn_nr < 0) {
        av_log(NULL, AV_LOG_WARNING, "Invalid conn_nr (pool_get_context)\n");
        return;
    }
    data = &thr_data[conn_nr];
    *out = data->out;
}

void pool_init() {
    thread_pool = pool_start(thr_io_close, 20);
}