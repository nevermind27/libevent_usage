#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdbool.h>
#include <curl/curl.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/util.h>
#include <event2/event.h>

static const char MESSAGE[] = "Message from server\n";
static const int TIMEOUT = 20;
static const unsigned short PORT = 3000;

static void listener_cb(struct evconnlistener *, evutil_socket_t,
    struct sockaddr *, int socklen, void *);
static void conn_writecb(struct bufferevent *, void *);
static void conn_eventcb(struct bufferevent *, short, void *);
static void signal_cb(evutil_socket_t, short, void *);

int
main(int argc, char **argv)
{
	struct event_base *base;
	struct evconnlistener *listener;
	struct event *signal_event;

	struct sockaddr_in sin = {0};

	base = event_base_new();
	if (!base) {
		fprintf(stderr, "Could not initialize libevent!\n");
		return 1;
	}

	sin.sin_family = AF_INET;
	sin.sin_port = htons(PORT);

	listener = evconnlistener_new_bind(base, listener_cb, (void *)base,
	    LEV_OPT_REUSEABLE|LEV_OPT_CLOSE_ON_FREE, -1,
	    (struct sockaddr*)&sin,
	    sizeof(sin));

	if (!listener) {
		fprintf(stderr, "Could not create a listener!\n");
		return 1;
	}

	signal_event = evsignal_new(base, SIGINT, signal_cb, (void *)base);

	if (!signal_event || event_add(signal_event, NULL)<0) {
		fprintf(stderr, "Could not create/add a signal event!\n");
		return 1;
	}

	event_base_dispatch(base);

	evconnlistener_free(listener);
	event_free(signal_event);
	event_base_free(base);

	printf("done\n");
	return 0;
}

struct RequestData {
    const char* url;
    int timeout;
	double total_time;
};

void* request_thread(void* arg) {
    struct RequestData* data = (struct RequestData*)arg;

    CURL* curl = curl_easy_init();
    if (curl) {
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, false); 
        curl_easy_setopt(curl, CURLOPT_URL, data->url);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, data->timeout);
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
			curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &data->total_time);
            printf("Request to %s successful\n", data->url);

        } else {
            fprintf(stderr, "Failed to request %s: %s\n", data->url, curl_easy_strerror(res));
        }

        curl_easy_cleanup(curl);
    } else {
        fprintf(stderr, "Failed to initialize CURL\n");
    }
    pthread_exit(NULL);
}

static void
listener_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *sa, int socklen, void *user_data)
{
	struct event_base *base = user_data;
	struct bufferevent *bev;

	bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE);
	if (!bev) {
		fprintf(stderr, "Error constructing bufferevent!");
		event_base_loopbreak(base);
		return;
	}
	bufferevent_setcb(bev, NULL, conn_writecb, conn_eventcb, NULL);
	bufferevent_enable(bev, EV_WRITE);
	bufferevent_disable(bev, EV_READ);
	bufferevent_write(bev, MESSAGE, strlen(MESSAGE));


}

static void
conn_writecb(struct bufferevent *bev, void *user_data)
{
	pthread_t threads[3];

    struct RequestData* data[3];
    for (int i = 0; i < 3; i++) {
        data[i] = (struct RequestData*)malloc(sizeof(struct RequestData));
        data[i]->url =  "https://demo-test-task-delayed.fly.dev/data";
        data[i]->timeout = TIMEOUT;

        if (pthread_create(&threads[i], NULL, request_thread, data[i]) != 0) {
            fprintf(stderr, "Failed to create thread for request\n");
        }
    }

	struct evbuffer *output = bufferevent_get_output(bev);
    for (int i = 0; i < 3; i++) {
        pthread_join(threads[i], NULL);
    
		if (i == 0) {
			evbuffer_add_printf(output, "{\"timeouts\":[%f,%f,%f]}\n", data[0]->total_time, data[1]->total_time, data[2]->total_time);
			evbuffer_drain(output,evbuffer_get_length(output));
		}
		free(data[i]);
    }
    if (evbuffer_get_length(output) == 0) {
        printf("flushed answer\n");
        bufferevent_free(bev);
    }
}


static void
conn_eventcb(struct bufferevent *bev, short events, void *user_data)
{
	if (events & BEV_EVENT_EOF) {
		printf("Connection closed.\n");
	} else if (events & BEV_EVENT_ERROR) {
		printf("Got an error on the connection: %s\n",
		    strerror(errno));
	}
	bufferevent_free(bev);
}

static void
signal_cb(evutil_socket_t sig, short events, void *user_data)
{
	struct event_base *base = user_data;
	struct timeval delay = { 2, 0 };

	printf("Caught an interrupt signal; exiting cleanly in two seconds.\n");

	event_base_loopexit(base, &delay);
}

