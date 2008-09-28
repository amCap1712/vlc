/*****************************************************************************
 * vout_internal.h : Internal vout definitions
 *****************************************************************************
 * Copyright (C) 2008 the VideoLAN team
 * Copyright (C) 2008 Laurent Aimar
 * $Id$
 *
 * Authors: Laurent Aimar < fenrir _AT_ videolan _DOT_ org >
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/


#if defined(__PLUGIN__) || defined(__BUILTIN__) || !defined(__LIBVLC__)
# error This header file can only be included from LibVLC.
#endif

#ifndef _VOUT_INTERNAL_H
#define _VOUT_INTERNAL_H 1

struct vout_thread_sys_t
{
    /* */
    vlc_mutex_t         vfilter_lock;         /**< video filter2 change lock */

    /* */
    uint32_t            render_time;           /**< last picture render time */
    unsigned int        i_par_num;           /**< monitor pixel aspect-ratio */
    unsigned int        i_par_den;           /**< monitor pixel aspect-ratio */

    /* */
    bool                b_direct;            /**< rendered are like direct ? */
    filter_t           *p_chroma;

    /**
     * These numbers are not supposed to be accurate, but are a
     * good indication of the thread status */
    count_t       c_fps_samples;                         /**< picture counts */
    mtime_t       p_fps_sample[VOUT_FPS_SAMPLES];     /**< FPS samples dates */

#if 0
    /* Statistics */
    count_t         c_loops;
    count_t         c_pictures, c_late_pictures;
    mtime_t         display_jitter;    /**< average deviation from the PTS */
    count_t         c_jitter_samples;  /**< number of samples used
                                           for the calculation of the jitter  */
#endif

    /* Pause */
    bool            b_paused;
    mtime_t         i_pause_date;

    /** delay created by internal caching */
    int             i_pts_delay;

    /* Filter chain */
    char           *psz_filter_chain;
    bool            b_filter_change;

    /* Video filter2 chain */
    filter_chain_t *p_vf2_chain;
    char           *psz_vf2;

    /* Misc */
    bool            b_snapshot;     /**< take one snapshot on the next loop */

    /* Show media title on videoutput */
    bool            b_title_show;
    mtime_t         i_title_timeout;
    int             i_title_position;
};

/* DO NOT use vout_RenderPicture unless you are in src/video_ouput */
picture_t *vout_RenderPicture( vout_thread_t *, picture_t *,
                               subpicture_t *, bool b_paused );

/* DO NOT use vout_CountPictureAvailable unless your are in src/input/decoder.c (no exception) */
int vout_CountPictureAvailable( vout_thread_t * );

/**
 * This function will (un)pause the display of pictures.
 * It is thread safe
 */
void vout_ChangePause( vout_thread_t *, bool b_paused, mtime_t i_date );

#endif

