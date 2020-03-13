/*****************************************************************************
 * listenbrainz.c : ListenBrainz submission plugin
 * ListenBrainz Submit Listens API 1
 * https://api.listenbrainz.org/1/submit-listens
 *****************************************************************************
 * Copyright (C) 2020 VLC authors and VideoLAN
 *
 * Author: Kartik Ohri <kartikohri13 at gmail dot com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <time.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>
#include <vlc_input_item.h>
#include <vlc_dialog.h>
#include <vlc_meta.h>
#include <vlc_memstream.h>
#include <vlc_stream.h>
#include <vlc_url.h>
#include <vlc_tls.h>
#include <vlc_player.h>
#include <vlc_playlist.h>
#include <vlc_vector.h>
#include <vlc_interrupt.h>

typedef struct listen_t {
    char *psz_artist;
    char *psz_title;
    char *psz_album;
    char *psz_track_number;
    char *psz_musicbrainz_recording_id;
    int i_length;
    time_t date;
} listen_t;

typedef struct VLC_VECTOR (listen_t) vlc_vector_listen_t;

struct intf_sys_t {
    vlc_vector_listen_t queue;

    vlc_player_t *player;
    struct vlc_player_listener_id *player_listener;
    struct vlc_player_timer_id *timer_listener;

    vlc_mutex_t lock;
    vlc_cond_t wait;                // song to submit event
    vlc_thread_t thread;            // thread to submit song
    vlc_interrupt_t *interrupt;
    bool live;

    vlc_url_t p_submit_url;         // where to submit data
    char *psz_user_token;           // authentication token

    listen_t p_current_song;
    bool b_meta_read;               // check if song metadata is already read
    vlc_tick_t time_played;
};

static int Open (vlc_object_t *);

static void Close (vlc_object_t *);

static void *Run (void *);

#define USER_TOKEN_TEXT      N_("User token")
#define USER_TOKEN_LONGTEXT  N_("The user token of your ListenBrainz account")
#define URL_TEXT             N_("Submission URL")
#define URL_LONGTEXT         N_("The URL set for an alternative ListenBrainz instance")

vlc_module_begin ()
    set_category(CAT_INTERFACE)
    set_subcategory(SUBCAT_INTERFACE_CONTROL)
    set_shortname (N_ ("ListenBrainz"))
    set_description (N_ ("Submit listens to ListenBrainz"))
    add_string("listenbrainz-user-token" , "" , USER_TOKEN_TEXT , USER_TOKEN_LONGTEXT , false)
    add_string("listenbrainz-submission-url" , "api.listenbrainz.org" , URL_TEXT , URL_LONGTEXT , false)
    set_capability("interface" , 0)
    set_callbacks(Open , Close)
vlc_module_end ()

static void DeleteSong (listen_t *p_song)
{
    FREENULL (p_song->psz_artist);
    FREENULL (p_song->psz_album);
    FREENULL (p_song->psz_title);
    FREENULL (p_song->psz_musicbrainz_recording_id);
    FREENULL (p_song->psz_track_number);
    p_song->i_length = 0;
    p_song->date = 0;
}

static void DeleteSongQueue(intf_thread_t *p_this)
{
    intf_sys_t *p_sys = p_this->p_sys;

    for (size_t i = 0; i < p_sys->queue.size; ++i)
        DeleteSong(&p_sys->queue.data[i]);
    vlc_vector_clear (&p_sys->queue);
}

static void ReadMetaData (intf_thread_t *p_this , input_item_t *item)
{
    intf_sys_t *p_sys = p_this->p_sys;

    if (item == NULL)
        return;

    vlc_mutex_lock (&p_sys->lock);

    p_sys->b_meta_read = true;
    time (&p_sys->p_current_song.date);

/* The retrieved metadata is encoded and then decoded to avoid UTF errors
 * while sending the JSON payload.*/
#define RETRIEVE_METADATA(a , b) do {                                   \
        char *psz_data = input_item_Get##b(item);                       \
        if (psz_data && *psz_data) {                                    \
            free(a);                                                    \
            a = vlc_uri_decode(vlc_uri_encode(psz_data));               \
        }                                                               \
        free(psz_data);                                                 \
    } while (0)

    RETRIEVE_METADATA(p_sys->p_current_song.psz_artist , AlbumArtist);
    if (!p_sys->p_current_song.psz_artist)
    {
        RETRIEVE_METADATA(p_sys->p_current_song.psz_artist , Artist);
        if (!p_sys->p_current_song.psz_artist)
        {
            DeleteSong (&p_sys->p_current_song);
            goto error;
        }
    }

    RETRIEVE_METADATA(p_sys->p_current_song.psz_title , Title);
    if (!p_sys->p_current_song.psz_title)
    {
        DeleteSong (&p_sys->p_current_song);
        goto error;
    }

    RETRIEVE_METADATA(p_sys->p_current_song.psz_album , Album);
    RETRIEVE_METADATA(p_sys->p_current_song.psz_musicbrainz_recording_id , TrackID);
    RETRIEVE_METADATA(p_sys->p_current_song.psz_track_number , TrackNum);
    p_sys->p_current_song.i_length = SEC_FROM_VLC_TICK (input_item_GetDuration (item));
    msg_Dbg (p_this , "Meta data registered");
    vlc_cond_signal (&p_sys->wait);

error:
    vlc_mutex_unlock (&p_sys->lock);

#undef RETRIEVE_METADATA
}

static void Enqueue (intf_thread_t *p_this)
{
    intf_sys_t *p_sys = p_this->p_sys;

    p_sys->b_meta_read = false;
    /* Song not yet initialized */
    if(p_sys->p_current_song.date == 0)
      return;
    vlc_mutex_lock (&p_sys->lock);

    if (EMPTY_STR(p_sys->p_current_song.psz_artist) ||
        EMPTY_STR(p_sys->p_current_song.psz_title))
    {
        msg_Dbg (p_this , "Missing artist or title, not submitting");
        goto error;
    }

    if (p_sys->p_current_song.i_length == 0)
        p_sys->p_current_song.i_length = p_sys->time_played;

    if (p_sys->time_played < 30)
    {
        msg_Dbg (p_this , "Song not listened long enough, not submitting");
        goto error;
    }

    msg_Dbg (p_this , "Song will be submitted.");
    /* Transfer the ownership of allocated datas to the queue */
    vlc_vector_push (&p_sys->queue , p_sys->p_current_song);
    memset(&p_sys->p_current_song, 0, sizeof(p_sys->p_current_song));

    vlc_cond_signal (&p_sys->wait);
    vlc_mutex_unlock (&p_sys->lock);
    return;

error:
    DeleteSong (&p_sys->p_current_song);
    vlc_mutex_unlock (&p_sys->lock);
}

static void PlayerStateChanged (vlc_player_t *player , enum vlc_player_state state , void *data)
{
    intf_thread_t *intf = data;
    intf_sys_t *p_sys = intf->p_sys;

    if (vlc_player_GetVideoTrackCount (player))
        return;

    if (!p_sys->b_meta_read && state >= VLC_PLAYER_STATE_PLAYING)
    {
        input_item_t *item = vlc_player_GetCurrentMedia (p_sys->player);
        ReadMetaData (intf , item);
        return;
    }

    if (state == VLC_PLAYER_STATE_STOPPED)
        Enqueue (intf);
}

static void OnTimerUpdate (const struct vlc_player_timer_point *value , void *data)
{
    intf_thread_t *intf = data;
    intf_sys_t *p_sys = intf->p_sys;
    p_sys->time_played = SEC_FROM_VLC_TICK (value->ts - VLC_TICK_0);
}

static void OnTimerStopped (vlc_tick_t system_date , void *data)
{
    (void) system_date;
    (void) data;
}

static void OnCurrentMediaChanged (vlc_player_t *player , input_item_t *new_media , void *data)
{
    intf_thread_t *intf = data;
    Enqueue (intf);

    intf_sys_t *p_sys = intf->p_sys;
    p_sys->b_meta_read = false;

    if (!new_media || vlc_player_GetVideoTrackCount (player))
        return;

    p_sys->time_played = 0;
    if (input_item_IsPreparsed (new_media))
        ReadMetaData (intf , new_media);
}

static char *PreparePayload (intf_thread_t *p_this)
{
    intf_sys_t *p_sys = p_this->p_sys;
    struct vlc_memstream payload;
    vlc_memstream_open (&payload);

    if (p_sys->queue.size == 1)
        vlc_memstream_printf (&payload , "{\"listen_type\":\"single\",\"payload\":[");
    else
        vlc_memstream_printf (&payload , "{\"listen_type\":\"import\",\"payload\":[");

    for (int i_song = 0 ; i_song < ( int ) p_sys->queue.size ; i_song++)
    {
        listen_t *p_song = &p_sys->queue.data[ i_song ];

        vlc_memstream_printf (&payload , "{\"listened_at\": %"PRIu64 , ( uint64_t ) p_song->date);
        vlc_memstream_printf (&payload , ", \"track_metadata\": {\"artist_name\": \"%s\", " ,
                              p_song->psz_artist);
        vlc_memstream_printf (&payload , " \"track_name\": \"%s\", " , p_song->psz_title);
        if (!EMPTY_STR (p_song->psz_album))
            vlc_memstream_printf (&payload , " \"release_name\": \"%s\"" , p_song->psz_album);
        if (!EMPTY_STR (p_song->psz_musicbrainz_recording_id))
            vlc_memstream_printf (&payload , ", \"additional_info\": {\"recording_mbid\":\"%s\"} " ,
                                  p_song->psz_musicbrainz_recording_id);
        vlc_memstream_printf (&payload , "}}");
    }

    vlc_memstream_printf (&payload , "]}");

    int i_status = vlc_memstream_close (&payload);
    if (!i_status)
    {
        msg_Dbg (p_this , "Payload: %s" , payload.ptr);
        return payload.ptr;
    }
    else
        return NULL;
}

static char *PrepareRequest (intf_thread_t *p_this , char *payload)
{
    intf_sys_t *p_sys = p_this->p_sys;
    struct vlc_memstream request;

    vlc_memstream_open (&request);
    vlc_memstream_printf (&request , "POST %s HTTP/1.1\r\n" , p_sys->p_submit_url.psz_path);
    vlc_memstream_printf (&request , "Host: %s\r\n" , p_sys->p_submit_url.psz_host);
    vlc_memstream_printf (&request , "Authorization: Token %s\r\n" , p_sys->psz_user_token);
    vlc_memstream_puts (&request , "User-Agent: "PACKAGE"/"VERSION"\r\n");
    vlc_memstream_puts (&request , "Connection: close\r\n");
    vlc_memstream_puts (&request , "Accept-Encoding: identity\r\n");
    vlc_memstream_printf (&request , "Content-Length: %zu\r\n" , strlen (payload));
    vlc_memstream_puts (&request , "\r\n");
    vlc_memstream_puts (&request , payload);
    vlc_memstream_puts (&request , "\r\n\r\n");

    free (payload);

    int i_status = vlc_memstream_close (&request);
    if (!i_status)
        return request.ptr;
    else
        return NULL;
}

static int SendRequest (intf_thread_t *p_this , char *request)
{
    uint8_t p_buffer[1024];
    int i_ret;

    intf_sys_t *p_sys = p_this->p_sys;
    vlc_tls_client_t *creds = vlc_tls_ClientCreate (VLC_OBJECT (p_this));
    vlc_tls_t *sock = vlc_tls_SocketOpenTLS (creds , p_sys->p_submit_url.psz_host , 443 , NULL , NULL , NULL);

    if (sock == NULL)
    {
        vlc_tls_ClientDelete(creds);
        return VLC_EGENERIC;
    }

    i_ret = vlc_tls_Write (sock , request , strlen (request));

    if (i_ret == -1)
    {
        vlc_tls_Close (sock);
        vlc_tls_ClientDelete(creds);
        return VLC_EGENERIC;
    }

    i_ret = vlc_tls_Read (sock , p_buffer , sizeof (p_buffer) - 1 , false);
    msg_Dbg (p_this , "Response: %s" , ( char * ) p_buffer);
    vlc_tls_Close (sock);
    vlc_tls_ClientDelete(creds);
    if (i_ret <= 0)
    {
        msg_Warn (p_this , "No response");
        return VLC_EGENERIC;
    }
    p_buffer[ i_ret ] = '\0';

    char *status = strchr((char *)p_buffer, '\n');
    *status = 0;

    if (strstr (( char * ) p_buffer , "200"))
    {
        msg_Dbg (p_this , "Submission successful!");
        return VLC_SUCCESS;
    }
    else if (strstr (( char * ) p_buffer , "401"))
        msg_Warn (p_this , "Authentication Error");
    else
        msg_Warn (p_this , "Invalid Request");

    return VLC_EGENERIC;
}

static int Configure (intf_thread_t *p_intf)
{
    int i_ret;
    char *psz_submission_url , *psz_url;
    intf_sys_t *p_sys = p_intf->p_sys;

    p_sys->psz_user_token = var_InheritString (p_intf , "listenbrainz-user-token");
    if (EMPTY_STR (p_sys->psz_user_token))
    {
        vlc_dialog_display_error (p_intf ,
                                  _ ("ListenBrainz User Token not set") , "%s" ,
                                  _ ("Please set a user token or disable the ListenBrainz plugin, and restart VLC.\n"
                                     " Visit https://listenbrainz.org/profile/ to get a user token."));
        return VLC_EGENERIC;
    }

    psz_submission_url = var_InheritString (p_intf , "listenbrainz-submission-url");
    if (psz_submission_url)
    {
        i_ret = asprintf (&psz_url , "https://%s/1/submit-listens" , psz_submission_url);
        free (psz_submission_url);
        if (i_ret != -1)
        {
            vlc_UrlParse (&p_sys->p_submit_url , psz_url);
            free (psz_url);
            return VLC_SUCCESS;
        }
    }

    vlc_dialog_display_error (p_intf ,
                              _ ("ListenBrainz API URL Invalid") , "%s" ,
                              _ ("Please set a valid endpoint URL. The default value is api.listenbrainz.org ."));
    return VLC_EGENERIC;
}

static int Open (vlc_object_t *p_this)
{
    intf_thread_t *p_intf = ( intf_thread_t * ) p_this;
    intf_sys_t *p_sys = calloc (1 , sizeof (intf_sys_t));

    if (!p_sys)
        return VLC_ENOMEM;

    p_intf->p_sys = p_sys;
    p_sys->live = true;

    if (Configure (p_intf) != VLC_SUCCESS)
        goto error;

    static struct vlc_player_cbs const player_cbs =
        {
            .on_state_changed = PlayerStateChanged ,
            .on_current_media_changed = OnCurrentMediaChanged ,
        };
    static struct vlc_player_timer_cbs const timer_cbs =
        {
            .on_update = OnTimerUpdate ,
            .on_discontinuity = OnTimerStopped ,
        };

    vlc_playlist_t *playlist = vlc_intf_GetMainPlaylist (p_intf);
    p_sys->player = vlc_playlist_GetPlayer (playlist);

    vlc_player_Lock (p_sys->player);
    p_sys->player_listener = vlc_player_AddListener (p_sys->player , &player_cbs , p_intf);
    vlc_player_Unlock (p_sys->player);

    if (!p_sys->player_listener)
        goto error;

    p_sys->timer_listener = vlc_player_AddTimer (p_sys->player , VLC_TICK_FROM_SEC (1) , &timer_cbs , p_intf);
    if (!p_sys->timer_listener)
        goto error;

    p_sys->interrupt = vlc_interrupt_create();
    if (unlikely(p_sys->interrupt == NULL))
        goto error;

    vlc_mutex_init (&p_sys->lock);
    vlc_cond_init (&p_sys->wait);

    if (vlc_clone (&p_sys->thread , Run , p_intf , VLC_THREAD_PRIORITY_LOW))
    {
        vlc_interrupt_destroy(p_sys->interrupt);
        goto error;
    }

    vlc_vector_init(&p_sys->queue);
    return VLC_SUCCESS;

error:
    if (p_sys->player_listener)
    {
        vlc_player_Lock (p_sys->player);
        vlc_player_RemoveListener (p_sys->player , p_sys->player_listener);
        vlc_player_Unlock (p_sys->player);
    }
    if (p_sys->timer_listener)
        vlc_player_RemoveTimer (p_sys->player , p_sys->timer_listener);
    vlc_UrlClean (&p_sys->p_submit_url);
    free(p_sys->psz_user_token);
    free (p_sys);
    return VLC_EGENERIC;
}

static void Close (vlc_object_t *p_this)
{
    intf_thread_t *p_intf = ( intf_thread_t * ) p_this;
    intf_sys_t *p_sys = p_intf->p_sys;

    vlc_mutex_lock(&p_sys->lock);
    p_sys->live = false;
    vlc_cond_signal (&p_sys->wait);
    vlc_mutex_unlock(&p_sys->lock);

    vlc_interrupt_kill(p_sys->interrupt);
    vlc_join (p_sys->thread , NULL);
    vlc_interrupt_destroy(p_sys->interrupt);

    vlc_player_Lock (p_sys->player);
    vlc_player_RemoveListener (p_sys->player , p_sys->player_listener);
    vlc_player_Unlock (p_sys->player);

    vlc_player_RemoveTimer (p_sys->player , p_sys->timer_listener);

    DeleteSongQueue(p_intf);
    DeleteSong(&p_sys->p_current_song);

    vlc_UrlClean (&p_sys->p_submit_url);
    free(p_sys->psz_user_token);

    free (p_sys);
}

static void *Run (void *data)
{
    intf_thread_t *p_intf = data;
    bool b_wait = 0;
    char *request , *payload;

    intf_sys_t *p_sys = p_intf->p_sys;

    vlc_interrupt_set(p_sys->interrupt);

    vlc_mutex_lock (&p_sys->lock);
    for (;;)
    {
        if (b_wait)
        {
            vlc_tick_t deadline = vlc_tick_now() + VLC_TICK_FROM_SEC (60);
            int ret = 0;
            while (p_sys->live && ret == 0) // wait for 1 min
                ret = vlc_cond_timedwait (&p_sys->wait , &p_sys->lock, deadline);
        }

        while (p_sys->live && p_sys->queue.size == 0)
            vlc_cond_wait (&p_sys->wait , &p_sys->lock);

        if (!p_sys->live)
            break;

        payload = PreparePayload (p_intf);
        vlc_mutex_unlock (&p_sys->lock);

        if (!payload)
        {
            msg_Warn (p_intf , "Error: Unable to generate payload");
            return NULL;
        }

        request = PrepareRequest (p_intf , payload);
        if (!request)
        {
            msg_Warn (p_intf , "Error: Unable to generate request body");
            return NULL;
        }

        int ret = SendRequest (p_intf , request);
        free(request);

        vlc_mutex_lock (&p_sys->lock);

        if (ret == VLC_SUCCESS)
        {
            DeleteSongQueue(p_intf);
            b_wait = false;
        }
        else
        {
            msg_Warn (p_intf , "Error: Could not transmit request");
            b_wait = true;
        }
    }

    vlc_mutex_unlock (&p_sys->lock);
    return NULL;
}

