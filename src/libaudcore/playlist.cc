/*
 * playlist.cc
 * Copyright 2009-2017 John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include "playlist-internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib/gstdio.h>

#include "audstrings.h"
#include "drct.h"
#include "hook.h"
#include "i18n.h"
#include "internal.h"
#include "list.h"
#include "mainloop.h"
#include "multihash.h"
#include "parse.h"
#include "playlist-data.h"
#include "runtime.h"
#include "threads.h"

enum
{
    ResumeStop,
    ResumePlay,
    ResumePause
};

/* update hooks */
enum
{
    SetActive = (1 << 0),
    SetPlaying = (1 << 1),
    PlaybackBegin = (1 << 2),
    PlaybackStop = (1 << 3)
};

enum class UpdateState
{
    None,
    Delayed,
    Queued
};

#define STATE_FILE "playlist-state"

#define ENTER_GET_PLAYLIST(...)                                                \
    auto mh = mutex.take();                                                    \
    PlaylistData * playlist = m_id ? m_id->data : nullptr;                     \
    if (!playlist)                                                             \
    return __VA_ARGS__

#define SIMPLE_WRAPPER(type, failcode, func, ...)                              \
    ENTER_GET_PLAYLIST(failcode);                                              \
    return playlist->func(__VA_ARGS__)

#define SIMPLE_VOID_WRAPPER(func, ...)                                         \
    ENTER_GET_PLAYLIST();                                                      \
    playlist->func(__VA_ARGS__)

static const char * const default_title = N_("New Playlist");
static const char * const temp_title = N_("Now Playing");

static aud::mutex mutex;
static aud::condvar condvar;

/*
 * Each playlist is associated with its own ID struct, which contains a unique
 * integer "stamp" (this is the source of the internal filenames 1000.audpl,
 * 1001.audpl, etc.)  The ID struct also serves as a "weak" pointer to the
 * actual data, and persists even after the playlist itself is destroyed.
 * The IDs are stored in a hash table, allowing lookup by stamp.
 *
 * In brief: Playlist (public handle)
 *             points to ->
 *           Playlist::ID (unique ID / weak pointer)
 *             points to ->
 *           PlaylistData (actual playlist data)
 */
struct Playlist::ID
{
    int stamp;           // integer stamp, determines filename
    int index;           // display order
    PlaylistData * data; // pointer to actual playlist data
};

static SimpleHash<IntHashKey, Playlist::ID> id_table;
static int next_stamp = 1000;

static Index<SmartPtr<PlaylistData>> playlists;
static Playlist::ID * active_id = nullptr;
static Playlist::ID * playing_id = nullptr;
static int resume_playlist = -1;
static bool resume_paused = false;

static QueuedFunc queued_update;
static Playlist::UpdateLevel update_level;
static int update_hooks;
static UpdateState update_state;

struct ScanItem : public ListNode
{
    ScanItem(PlaylistData * playlist, PlaylistEntry * entry,
             ScanRequest * request, bool for_playback)
        : playlist(playlist), entry(entry), request(request),
          for_playback(for_playback), handled_by_playback(false)
    {
    }

    PlaylistData * playlist;
    PlaylistEntry * entry;
    ScanRequest * request;
    bool for_playback;
    bool handled_by_playback;
};

static bool scan_enabled_nominal, scan_enabled;
static int scan_playlist, scan_row;
static List<ScanItem> scan_list;

static void scan_finish(ScanRequest * request);
static void scan_cancel(PlaylistEntry * entry);
static void scan_restart();

/* creates a new playlist with the requested stamp (if not already in use) */
static Playlist::ID * create_playlist(int stamp)
{
    Playlist::ID * id;

    if (stamp >= 0 && !id_table.lookup(stamp))
        id = id_table.add(stamp, {stamp, -1, nullptr});
    else
    {
        while (id_table.lookup(next_stamp))
            next_stamp++;

        id = id_table.add(next_stamp, {next_stamp, -1, nullptr});
    }

    id->data = new PlaylistData(id, _(default_title));

    return id;
}

static void number_playlists(int at, int length)
{
    for (int i = at; i < at + length; i++)
        playlists[i]->id()->index = i;
}

static void update(void *)
{
    auto mh = mutex.take();

    int hooks = update_hooks;
    auto level = update_level;

    Index<PlaylistEx> position_change_list;

    for (auto & p : playlists)
    {
        bool position_changed = false;
        p->swap_updates(position_changed);

        if (position_changed)
            position_change_list.append(p->id());
    }

    update_hooks = 0;
    update_level = Playlist::NoUpdate;
    update_state = UpdateState::None;
    event_queue_unpause();

    mh.unlock();

    if (level != Playlist::NoUpdate)
        hook_call("playlist update", aud::to_ptr(level));

    for (PlaylistEx playlist : position_change_list)
        hook_call("playlist position", aud::to_ptr(playlist));

    if ((hooks & SetActive))
        hook_call("playlist activate", nullptr);
    if ((hooks & SetPlaying))
        hook_call("playlist set playing", nullptr);
    if ((hooks & PlaybackBegin))
        hook_call("playback begin", nullptr);
    if ((hooks & PlaybackStop))
        hook_call("playback stop", nullptr);
}

static void queue_update()
{
    if (update_state < UpdateState::Queued)
    {
        event_queue_pause(); // give playlist updates priority
        queued_update.queue(update, nullptr);
        update_state = UpdateState::Queued;
    }
}

static void queue_update_hooks(int hooks)
{
    if ((hooks & PlaybackBegin))
        update_hooks &= ~PlaybackStop;
    if ((hooks & PlaybackStop))
        update_hooks &= ~PlaybackBegin;

    update_hooks |= hooks;
    queue_update();
}

static void queue_global_update(Playlist::UpdateLevel level, int flags = 0)
{
    if (level == Playlist::Structure)
        scan_restart();

    if ((flags & PlaylistData::DelayedUpdate))
    {
        if (update_state < UpdateState::Delayed)
        {
            queued_update.queue(250, update, nullptr);
            update_state = UpdateState::Delayed;
        }
    }
    else
    {
        queue_update();
    }

    update_level = aud::max(update_level, level);
}

EXPORT bool Playlist::update_pending_any()
{
    auto mh = mutex.take();
    return (update_level != Playlist::NoUpdate);
}

EXPORT void Playlist::process_pending_update() { update(nullptr); }

EXPORT bool Playlist::scan_in_progress() const
{
    ENTER_GET_PLAYLIST(false);
    return (playlist->scan_status != PlaylistData::NotScanning);
}

EXPORT bool Playlist::scan_in_progress_any()
{
    auto mh = mutex.take();

    for (auto & p : playlists)
    {
        if (p->scan_status != PlaylistData::NotScanning)
            return true;
    }

    return false;
}

static ScanItem * scan_list_find_entry(PlaylistEntry * entry)
{
    auto match = [entry](const ScanItem & item) { return item.entry == entry; };

    return scan_list.find(match);
}

static void scan_queue_entry(PlaylistData * playlist, PlaylistEntry * entry,
                             bool for_playback = false)
{
    int extra_flags = for_playback ? (SCAN_IMAGE | SCAN_FILE) : 0;
    auto request =
        playlist->create_scan_request(entry, scan_finish, extra_flags);

    scan_list.append(new ScanItem(playlist, entry, request, for_playback));

    /* playback entry will be scanned by the playback thread */
    if (!for_playback)
        scanner_request(request);
}

static void scan_reset_playback()
{
    auto match = [](const ScanItem & item) { return item.for_playback; };

    ScanItem * item = scan_list.find(match);
    if (!item)
        return;

    item->for_playback = false;

    /* if playback was canceled before the entry was scanned, requeue it */
    if (!item->handled_by_playback)
        scanner_request(item->request);
}

static void scan_check_complete(PlaylistData * playlist)
{
    auto match = [playlist](const ScanItem & item) {
        return item.playlist == playlist;
    };

    if (playlist->scan_status != PlaylistData::ScanEnding ||
        scan_list.find(match))
        return;

    playlist->scan_status = PlaylistData::NotScanning;

    if (update_state == UpdateState::Delayed)
        queue_update();

    event_queue_cancel("playlist scan complete");
    event_queue("playlist scan complete", nullptr);
}

static bool scan_queue_next_entry()
{
    if (!scan_enabled)
        return false;

    while (scan_playlist < playlists.len())
    {
        PlaylistData * playlist = playlists[scan_playlist].get();

        if (playlist->scan_status == PlaylistData::ScanActive)
        {
            while (1)
            {
                scan_row = playlist->next_unscanned_entry(scan_row);
                if (scan_row < 0)
                    break;

                auto entry = playlist->entry_at(scan_row);
                if (!scan_list_find_entry(entry))
                {
                    scan_queue_entry(playlist, entry);
                    return true;
                }

                scan_row++;
            }

            playlist->scan_status = PlaylistData::ScanEnding;
            scan_check_complete(playlist);
        }

        scan_playlist++;
        scan_row = 0;
    }

    return false;
}

static void scan_schedule()
{
    int scheduled = 0;

    for (ScanItem * item = scan_list.head(); item; item = scan_list.next(item))
    {
        if (++scheduled >= SCAN_THREADS)
            return;
    }

    while (scan_queue_next_entry())
    {
        if (++scheduled >= SCAN_THREADS)
            return;
    }
}

static void scan_finish(ScanRequest * request)
{
    auto mh = mutex.take();

    auto match = [request](const ScanItem & item) {
        return item.request == request;
    };

    ScanItem * item = scan_list.find(match);
    if (!item)
        return;

    PlaylistData * playlist = item->playlist;
    PlaylistEntry * entry = item->entry;

    scan_list.remove(item);

    // only use delayed update if a scan is still in progress
    int update_flags = 0;
    if (scan_enabled && playlist->scan_status != PlaylistData::NotScanning)
        update_flags = PlaylistData::DelayedUpdate;

    playlist->update_entry_from_scan(entry, request, update_flags);

    delete item;

    scan_check_complete(playlist);
    scan_schedule();

    condvar.notify_all();
}

static void scan_cancel(PlaylistEntry * entry)
{
    ScanItem * item = scan_list_find_entry(entry);
    if (!item)
        return;

    scan_list.remove(item);
    delete (item);
}

static void scan_restart()
{
    scan_playlist = 0;
    scan_row = 0;
    scan_schedule();
}

/* mutex may be unlocked during the call */
static void wait_for_entry(aud::mutex::holder & mh, PlaylistData * playlist,
                           int entry_num, bool need_decoder, bool need_tuple)
{
    bool scan_started = false;

    while (1)
    {
        PlaylistEntry * entry = playlist->entry_at(entry_num);

        // check whether entry is deleted or has already been scanned
        if (!entry ||
            !playlist->entry_needs_rescan(entry, need_decoder, need_tuple))
            return;

        // start scan if not already running ...
        if (!scan_list_find_entry(entry))
        {
            // ... but only once
            if (scan_started)
                return;

            scan_queue_entry(playlist, entry);
        }

        // wait for scan to finish
        scan_started = true;
        condvar.wait(mh);
    }
}

static void start_playback_locked(int seek_time, bool pause)
{
    art_clear_current();
    scan_reset_playback();

    playback_play(seek_time, pause);

    auto playlist = playing_id->data;
    auto entry = playlist->entry_at(playlist->position());

    // playback always begins with a rescan of the current entry in order to
    // open the file, ensure a valid tuple, and read album art
    scan_cancel(entry);
    scan_queue_entry(playlist, entry, true);
}

static void stop_playback_locked()
{
    art_clear_current();
    scan_reset_playback();

    playback_stop();
}

void pl_signal_entry_deleted(PlaylistEntry * entry) { scan_cancel(entry); }

void pl_signal_position_changed(Playlist::ID * id)
{
    queue_update();

    if (id == playing_id)
    {
        if (id->data->position() >= 0)
        {
            start_playback_locked(0, aud_drct_get_paused());
            queue_update_hooks(PlaybackBegin);
        }
        else
        {
            playing_id = nullptr;
            stop_playback_locked();
            queue_update_hooks(SetPlaying | PlaybackStop);
        }
    }
}

void pl_signal_update_queued(Playlist::ID * id, Playlist::UpdateLevel level,
                             int flags)
{
    auto playlist = id->data;

    if (level == Playlist::Structure)
        playlist->scan_status = PlaylistData::ScanActive;

    if (level >= Playlist::Metadata)
    {
        int pos = playlist->position();
        if (id == playing_id && pos >= 0)
            playback_set_info(pos, playlist->entry_tuple(pos));

        playlist->modified = true;
    }

    queue_global_update(level, flags);
}

void pl_signal_rescan_needed(Playlist::ID * id)
{
    id->data->scan_status = PlaylistData::ScanActive;
    scan_restart();
}

void pl_signal_playlist_deleted(Playlist::ID * id)
{
    /* break weak pointer link */
    id->data = nullptr;
    id->index = -1;
}

static void pl_hook_reformat_titles(void *, void *)
{
    auto mh = mutex.take();

    PlaylistData::update_formatter();

    for (auto & playlist : playlists)
        playlist->reformat_titles();
}

static void pl_hook_trigger_scan(void *, void *)
{
    auto mh = mutex.take();
    scan_enabled = scan_enabled_nominal && !aud_get_bool("metadata_on_play");
    scan_restart();
}

void playlist_init()
{
    srand(time(nullptr));

    auto mh = mutex.take();

    PlaylistData::update_formatter();

    update_level = Playlist::NoUpdate;
    update_hooks = 0;
    update_state = UpdateState::None;
    scan_enabled = scan_enabled_nominal = false;
    scan_playlist = scan_row = 0;

    mh.unlock();

    hook_associate("set generic_title_format", pl_hook_reformat_titles,
                   nullptr);
    hook_associate("set leading_zero", pl_hook_reformat_titles, nullptr);
    hook_associate("set metadata_fallbacks", pl_hook_reformat_titles, nullptr);
    hook_associate("set show_hours", pl_hook_reformat_titles, nullptr);
    hook_associate("set show_numbers_in_pl", pl_hook_reformat_titles, nullptr);
    hook_associate("set metadata_on_play", pl_hook_trigger_scan, nullptr);
}

void playlist_enable_scan(bool enable)
{
    auto mh = mutex.take();

    scan_enabled_nominal = enable;
    scan_enabled = scan_enabled_nominal && !aud_get_bool("metadata_on_play");
    scan_restart();
}

void playlist_clear_updates()
{
    auto mh = mutex.take();

    /* clear updates queued during init sequence */
    for (auto & playlist : playlists)
        playlist->cancel_updates();

    queued_update.stop();
    update_level = Playlist::NoUpdate;
    update_hooks = 0;
    update_state = UpdateState::None;
    event_queue_unpause();
}

void playlist_end()
{
    hook_dissociate("set generic_title_format", pl_hook_reformat_titles);
    hook_dissociate("set leading_zero", pl_hook_reformat_titles);
    hook_dissociate("set metadata_fallbacks", pl_hook_reformat_titles);
    hook_dissociate("set show_hours", pl_hook_reformat_titles);
    hook_dissociate("set show_numbers_in_pl", pl_hook_reformat_titles);
    hook_dissociate("set metadata_on_play", pl_hook_trigger_scan);

    playlist_cache_clear();

    auto mh = mutex.take();

    /* playback should already be stopped */
    assert(!playing_id);
    assert(!scan_list.head());

    queued_update.stop();

    active_id = nullptr;
    resume_playlist = -1;
    resume_paused = false;

    playlists.clear();
    id_table.clear();

    PlaylistData::cleanup_formatter();
}

EXPORT int Playlist::n_entries() const { SIMPLE_WRAPPER(int, 0, n_entries); }
EXPORT void Playlist::remove_entries(int at, int number) const
{
    SIMPLE_VOID_WRAPPER(remove_entries, at, number);
}
EXPORT String Playlist::entry_filename(int entry_num) const
{
    SIMPLE_WRAPPER(String, String(), entry_filename, entry_num);
}

EXPORT int Playlist::get_position() const { SIMPLE_WRAPPER(int, -1, position); }
EXPORT void Playlist::set_position(int entry_num) const
{
    SIMPLE_VOID_WRAPPER(set_position, entry_num);
}
EXPORT bool Playlist::prev_song() const
{
    SIMPLE_WRAPPER(bool, false, prev_song);
}
EXPORT bool Playlist::prev_album() const
{
    SIMPLE_WRAPPER(bool, false, prev_album);
}
EXPORT bool Playlist::next_song(bool repeat) const
{
    SIMPLE_WRAPPER(bool, false, next_song, repeat);
}
EXPORT bool Playlist::next_album(bool repeat) const
{
    SIMPLE_WRAPPER(bool, false, next_album, repeat);
}
EXPORT int Playlist::get_focus() const { SIMPLE_WRAPPER(int, -1, focus); }
EXPORT void Playlist::set_focus(int entry_num) const
{
    SIMPLE_VOID_WRAPPER(set_focus, entry_num);
}
EXPORT bool Playlist::entry_selected(int entry_num) const
{
    SIMPLE_WRAPPER(bool, false, entry_selected, entry_num);
}
EXPORT void Playlist::select_entry(int entry_num, bool selected) const
{
    SIMPLE_VOID_WRAPPER(select_entry, entry_num, selected);
}
EXPORT int Playlist::n_selected(int at, int number) const
{
    SIMPLE_WRAPPER(int, 0, n_selected, at, number);
}
EXPORT void Playlist::select_all(bool selected) const
{
    SIMPLE_VOID_WRAPPER(select_all, selected);
}
EXPORT int Playlist::shift_entries(int entry_num, int distance) const
{
    SIMPLE_WRAPPER(int, 0, shift_entries, entry_num, distance);
}
EXPORT void Playlist::remove_selected() const
{
    SIMPLE_VOID_WRAPPER(remove_selected);
}

EXPORT void Playlist::sort_by_filename(StringCompareFunc compare) const
{
    SIMPLE_VOID_WRAPPER(sort, {compare, nullptr});
}
EXPORT void Playlist::sort_by_tuple(TupleCompareFunc compare) const
{
    SIMPLE_VOID_WRAPPER(sort, {nullptr, compare});
}
EXPORT void Playlist::sort_selected_by_filename(StringCompareFunc compare) const
{
    SIMPLE_VOID_WRAPPER(sort_selected, {compare, nullptr});
}
EXPORT void Playlist::sort_selected_by_tuple(TupleCompareFunc compare) const
{
    SIMPLE_VOID_WRAPPER(sort_selected, {nullptr, compare});
}
EXPORT void Playlist::reverse_order() const
{
    SIMPLE_VOID_WRAPPER(reverse_order);
}
EXPORT void Playlist::reverse_selected() const
{
    SIMPLE_VOID_WRAPPER(reverse_selected);
}
EXPORT void Playlist::randomize_order() const
{
    SIMPLE_VOID_WRAPPER(randomize_order);
}
EXPORT void Playlist::randomize_selected() const
{
    SIMPLE_VOID_WRAPPER(randomize_selected);
}

EXPORT void Playlist::rescan_all() const
{
    SIMPLE_VOID_WRAPPER(reset_tuples, false);
}
EXPORT void Playlist::rescan_selected() const
{
    SIMPLE_VOID_WRAPPER(reset_tuples, true);
}

EXPORT int64_t Playlist::total_length_ms() const
{
    SIMPLE_WRAPPER(int64_t, 0, total_length);
}
EXPORT int64_t Playlist::selected_length_ms() const
{
    SIMPLE_WRAPPER(int64_t, 0, selected_length);
}

EXPORT int Playlist::n_queued() const { SIMPLE_WRAPPER(int, 0, n_queued); }
EXPORT void Playlist::queue_insert(int at, int entry_num) const
{
    SIMPLE_VOID_WRAPPER(queue_insert, at, entry_num);
}
EXPORT void Playlist::queue_insert_selected(int at) const
{
    SIMPLE_VOID_WRAPPER(queue_insert_selected, at);
}
EXPORT int Playlist::queue_get_entry(int at) const
{
    SIMPLE_WRAPPER(int, -1, queue_get_entry, at);
}
EXPORT int Playlist::queue_find_entry(int entry_num) const
{
    SIMPLE_WRAPPER(int, -1, queue_find_entry, entry_num);
}
EXPORT void Playlist::queue_remove(int at, int number) const
{
    SIMPLE_VOID_WRAPPER(queue_remove, at, number);
}
EXPORT void Playlist::queue_remove_selected() const
{
    SIMPLE_VOID_WRAPPER(queue_remove_selected);
}

EXPORT bool Playlist::update_pending() const
{
    SIMPLE_WRAPPER(bool, false, update_pending);
}
EXPORT Playlist::Update Playlist::update_detail() const
{
    SIMPLE_WRAPPER(Update, Update(), last_update);
}

void PlaylistEx::insert_flat_items(int at,
                                   Index<PlaylistAddItem> && items) const
{
    SIMPLE_VOID_WRAPPER(insert_items, at, std::move(items));
}

EXPORT int Playlist::index() const
{
    ENTER_GET_PLAYLIST(-1);
    return m_id->index;
}

EXPORT int PlaylistEx::stamp() const
{
    ENTER_GET_PLAYLIST(-1);
    return m_id->stamp;
}

EXPORT int Playlist::n_playlists()
{
    auto mh = mutex.take();
    return playlists.len();
}

EXPORT Playlist Playlist::by_index(int at)
{
    auto mh = mutex.take();
    Playlist::ID * id =
        (at >= 0 && at < playlists.len()) ? playlists[at]->id() : nullptr;
    return Playlist(id);
}

static Playlist::ID * insert_playlist_locked(int at, int stamp = -1)
{
    if (at < 0 || at > playlists.len())
        at = playlists.len();

    auto id = create_playlist(stamp);

    playlists.insert(at, 1);
    playlists[at].capture(id->data);

    number_playlists(at, playlists.len() - at);

    /* this will only happen at startup */
    if (!active_id)
        active_id = id;

    queue_global_update(Playlist::Structure);

    return id;
}

static Playlist::ID * get_blank_locked()
{
    if (!strcmp(active_id->data->title, _(default_title)) &&
        !active_id->data->n_entries())
        return active_id;

    return insert_playlist_locked(active_id->index + 1);
}

Playlist PlaylistEx::insert_with_stamp(int at, int stamp)
{
    auto mh = mutex.take();
    auto id = insert_playlist_locked(at, stamp);
    return Playlist(id);
}

EXPORT Playlist Playlist::insert_playlist(int at)
{
    auto mh = mutex.take();
    auto id = insert_playlist_locked(at);
    return Playlist(id);
}

EXPORT void Playlist::reorder_playlists(int from, int to, int count)
{
    auto mh = mutex.take();

    if (from < 0 || from + count > playlists.len() || to < 0 ||
        to + count > playlists.len() || count < 0)
        return;

    Index<SmartPtr<PlaylistData>> displaced;

    if (to < from)
        displaced.move_from(playlists, to, -1, from - to, true, false);
    else
        displaced.move_from(playlists, from + count, -1, to - from, true,
                            false);

    playlists.shift(from, to, count);

    if (to < from)
    {
        playlists.move_from(displaced, 0, to + count, from - to, false, true);
        number_playlists(to, from + count - to);
    }
    else
    {
        playlists.move_from(displaced, 0, from, to - from, false, true);
        number_playlists(from, to + count - from);
    }

    queue_global_update(Structure);
}

EXPORT void Playlist::remove_playlist() const
{
    ENTER_GET_PLAYLIST();

    int at = m_id->index;
    playlists.remove(at, 1);

    if (!playlists.len())
        playlists.append(create_playlist(-1)->data);

    number_playlists(at, playlists.len() - at);

    if (m_id == active_id)
    {
        int active_num = aud::min(at, playlists.len() - 1);
        active_id = playlists[active_num]->id();
        queue_update_hooks(SetActive);
    }

    if (m_id == playing_id)
    {
        playing_id = nullptr;
        stop_playback_locked();
        queue_update_hooks(SetPlaying | PlaybackStop);
    }

    queue_global_update(Structure);
}

EXPORT void Playlist::set_filename(const char * filename) const
{
    ENTER_GET_PLAYLIST();

    playlist->filename = String(filename);
    playlist->modified = true;

    queue_global_update(Metadata);
}

EXPORT String Playlist::get_filename() const
{
    ENTER_GET_PLAYLIST(String());
    return playlist->filename;
}

EXPORT void Playlist::set_title(const char * title) const
{
    ENTER_GET_PLAYLIST();

    playlist->title = String(title);
    playlist->modified = true;

    queue_global_update(Metadata);
}

EXPORT String Playlist::get_title() const
{
    ENTER_GET_PLAYLIST(String());
    return playlist->title;
}

void PlaylistEx::set_modified(bool modified) const
{
    ENTER_GET_PLAYLIST();
    playlist->modified = modified;
}

bool PlaylistEx::get_modified() const
{
    ENTER_GET_PLAYLIST(false);
    return playlist->modified;
}

EXPORT void Playlist::activate() const
{
    ENTER_GET_PLAYLIST();

    if (m_id != active_id)
    {
        active_id = m_id;
        queue_update_hooks(SetActive);
    }
}

EXPORT Playlist Playlist::active_playlist()
{
    auto mh = mutex.take();
    return Playlist(active_id);
}

EXPORT Playlist Playlist::new_playlist()
{
    auto mh = mutex.take();

    int at = active_id->index + 1;
    auto id = insert_playlist_locked(at);

    active_id = id;
    queue_update_hooks(SetActive);

    return Playlist(id);
}

static void set_playing_locked(Playlist::ID * id, bool paused)
{
    if (id == playing_id)
    {
        /* already playing, just need to pause/unpause */
        if (aud_drct_get_paused() != paused)
            aud_drct_pause();

        return;
    }

    if (playing_id)
        playing_id->data->resume_time = aud_drct_get_time();

    /* is there anything to play? */
    if (id && id->data->position() < 0 && !id->data->next_song(true))
        id = nullptr;

    playing_id = id;

    if (id)
    {
        start_playback_locked(id->data->resume_time, paused);
        queue_update_hooks(SetPlaying | PlaybackBegin);
    }
    else
    {
        stop_playback_locked();
        queue_update_hooks(SetPlaying | PlaybackStop);
    }
}

EXPORT void Playlist::start_playback(bool paused) const
{
    ENTER_GET_PLAYLIST();
    set_playing_locked(m_id, paused);
}

EXPORT void aud_drct_stop()
{
    auto mh = mutex.take();
    set_playing_locked(nullptr, false);
}

EXPORT Playlist Playlist::playing_playlist()
{
    auto mh = mutex.take();
    return Playlist(playing_id);
}

EXPORT Playlist Playlist::blank_playlist()
{
    auto mh = mutex.take();
    auto id = get_blank_locked();
    return Playlist(id);
}

EXPORT Playlist Playlist::temporary_playlist()
{
    auto mh = mutex.take();

    const char * title = _(temp_title);
    ID * id = nullptr;

    for (auto & playlist : playlists)
    {
        if (!strcmp(playlist->title, title))
        {
            id = playlist->id();
            break;
        }
    }

    if (!id)
    {
        id = get_blank_locked();
        id->data->title = String(title);
    }

    return Playlist(id);
}

EXPORT PluginHandle * Playlist::entry_decoder(int entry_num, GetMode mode,
                                              String * error) const
{
    ENTER_GET_PLAYLIST(nullptr);
    wait_for_entry(mh, playlist, entry_num, (mode == Wait), false);
    return playlist->entry_decoder(entry_num, error);
}

EXPORT Tuple Playlist::entry_tuple(int entry_num, GetMode mode,
                                   String * error) const
{
    ENTER_GET_PLAYLIST(Tuple());
    wait_for_entry(mh, playlist, entry_num, false, (mode == Wait));
    return playlist->entry_tuple(entry_num, error);
}

EXPORT void Playlist::rescan_file(const char * filename)
{
    auto mh = mutex.take();

    for (auto & playlist : playlists)
        playlist->reset_tuple_of_file(filename);
}

// called from playback thread
DecodeInfo playback_entry_read(int serial)
{
    auto mh = mutex.take();
    DecodeInfo dec;

    if (playback_check_serial(serial))
    {
        auto playlist = playing_id->data;
        auto entry = playlist->entry_at(playlist->position());

        ScanItem * item = scan_list_find_entry(entry);
        assert(item && item->for_playback);

        ScanRequest * request = item->request;
        item->handled_by_playback = true;

        mh.unlock();
        request->run();
        mh.lock();

        if (playback_check_serial(serial))
        {
            assert(playlist == playing_id->data);

            int pos = playlist->position();
            playback_set_info(pos, playlist->entry_tuple(pos));

            art_cache_current(request->filename, std::move(request->image_data),
                              std::move(request->image_file));

            dec.filename = request->filename;
            dec.ip = request->ip;
            dec.file = std::move(request->file);
            dec.error = std::move(request->error);
        }

        delete request;
    }

    return dec;
}

// called from playback thread
void playback_entry_set_tuple(int serial, Tuple && tuple)
{
    auto mh = mutex.take();

    if (playback_check_serial(serial))
        playing_id->data->update_playback_entry(std::move(tuple));
}

void playlist_save_state()
{
    /* get playback state before locking playlists */
    bool paused = aud_drct_get_paused();
    int time = aud_drct_get_time();

    auto mh = mutex.take();

    const char * user_dir = aud_get_path(AudPath::UserDir);
    StringBuf path = filename_build({user_dir, STATE_FILE});

    FILE * handle = g_fopen(path, "w");
    if (!handle)
        return;

    fprintf(handle, "active %d\n", active_id ? active_id->index : -1);
    fprintf(handle, "playing %d\n", playing_id ? playing_id->index : -1);

    for (auto & playlist : playlists)
    {
        fprintf(handle, "playlist %d\n", playlist->id()->index);

        if (playlist->filename)
            fprintf(handle, "filename %s\n", (const char *)playlist->filename);

        fprintf(handle, "position %d\n", playlist->position());

        /* save shuffle history */
        auto history = playlist->shuffle_history();

        for (int i = 0; i < history.len(); i += 16)
        {
            int count = aud::min(16, history.len() - i);
            auto list = int_array_to_str(&history[i], count);
            fprintf(handle, "shuffle %s\n", (const char *)list);
        }

        /* resume state is stored per-playlist for historical reasons */
        bool is_playing = (playlist->id() == playing_id);
        fprintf(handle, "resume-state %d\n",
                (is_playing && paused) ? ResumePause : ResumePlay);
        fprintf(handle, "resume-time %d\n",
                is_playing ? time : playlist->resume_time);
    }

    fclose(handle);
}

void playlist_load_state()
{
    auto mh = mutex.take();
    int playlist_num;

    const char * user_dir = aud_get_path(AudPath::UserDir);
    StringBuf path = filename_build({user_dir, STATE_FILE});

    FILE * handle = g_fopen(path, "r");
    if (!handle)
        return;

    TextParser parser(handle);

    if (parser.get_int("active", playlist_num))
    {
        if (playlist_num >= 0 && playlist_num < playlists.len())
            active_id = playlists[playlist_num]->id();

        parser.next();
    }

    if (parser.get_int("playing", resume_playlist))
        parser.next();

    while (parser.get_int("playlist", playlist_num) && playlist_num >= 0 &&
           playlist_num < playlists.len())
    {
        PlaylistData * playlist = playlists[playlist_num].get();

        parser.next();

        playlist->filename = parser.get_str("filename");
        if (playlist->filename)
            parser.next();

        int position = -1;
        if (parser.get_int("position", position))
        {
            playlist->set_position(position);
            parser.next();
        }

        /* restore shuffle history */
        Index<int> history;

        for (String list; (list = parser.get_str("shuffle")); parser.next())
        {
            auto split = str_list_to_index(list, ", ");
            for (auto & str : split)
                history.append(str_to_int(str));
        }

        if (history.len())
            playlist->shuffle_replay(history);

        /* resume state is stored per-playlist for historical reasons */
        int resume_state = ResumePlay;
        if (parser.get_int("resume-state", resume_state))
            parser.next();

        if (playlist_num == resume_playlist)
        {
            if (resume_state == ResumeStop)
                resume_playlist = -1;
            if (resume_state == ResumePause)
                resume_paused = true;
        }

        if (parser.get_int("resume-time", playlist->resume_time))
            parser.next();
    }

    fclose(handle);

    /* set initial focus and selection */
    for (auto & playlist : playlists)
    {
        int focus = playlist->position();
        if (focus < 0 && playlist->n_entries())
            focus = 0;

        if (focus >= 0)
        {
            playlist->set_focus(focus);
            playlist->select_entry(focus, true);
        }
    }
}

EXPORT void aud_resume()
{
    if (aud_get_bool("always_resume_paused"))
        resume_paused = true;

    Playlist::by_index(resume_playlist).start_playback(resume_paused);
}
