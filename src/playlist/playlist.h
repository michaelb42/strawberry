/*
 * Strawberry Music Player
 * This file was part of Clementine.
 * Copyright 2010, David Sansome <me@davidsansome.com>
 * Copyright 2018-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef PLAYLIST_H
#define PLAYLIST_H

#include "config.h"

#include <QtGlobal>
#include <QObject>
#include <QAbstractItemModel>
#include <QAbstractListModel>
#include <QPersistentModelIndex>
#include <QFuture>
#include <QList>
#include <QMap>
#include <QMultiMap>
#include <QMetaType>
#include <QVariant>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QColor>
#include <QRgb>

#include "core/song.h"
#include "core/tagreaderclient.h"
#include "covermanager/albumcoverloaderresult.h"
#include "playlistitem.h"
#include "playlistsequence.h"
#include "smartplaylists/playlistgenerator_fwd.h"

class QMimeData;
class QUndoStack;
class QTimer;

class CollectionBackend;
class PlaylistBackend;
class PlaylistFilter;
class Queue;
class TaskManager;
class InternetService;
class RadioService;

namespace PlaylistUndoCommands {
class InsertItems;
class MoveItems;
class ReOrderItems;
class RemoveItems;
class ShuffleItems;
class SortItems;
}  // namespace PlaylistUndoCommands

using ColumnAlignmentMap = QMap<int, Qt::Alignment>;
Q_DECLARE_METATYPE(Qt::Alignment)
Q_DECLARE_METATYPE(ColumnAlignmentMap)

// Objects that may prevent a song being added to the playlist.
// When there is something about to be inserted into it,
// Playlist notifies all of its listeners about the fact and every one of them picks 'invalid' songs.
class SongInsertVetoListener : public QObject {
  Q_OBJECT

 public:
  // Listener returns a list of 'invalid' songs.
  // 'old_songs' are songs that are currently in the playlist and 'new_songs' are the songs about to be added if nobody exercises a veto.
  virtual SongList AboutToInsertSongs(const SongList &old_songs, const SongList &new_songs) = 0;
};

class Playlist : public QAbstractListModel {
  Q_OBJECT

  friend class PlaylistUndoCommands::InsertItems;
  friend class PlaylistUndoCommands::RemoveItems;
  friend class PlaylistUndoCommands::MoveItems;
  friend class PlaylistUndoCommands::ReOrderItems;

 public:
  explicit Playlist(PlaylistBackend *backend, TaskManager *task_manager, CollectionBackend *collection, const int id, const QString &special_type = QString(), const bool favorite = false, QObject *parent = nullptr);
  ~Playlist() override;

  void SkipTracks(const QModelIndexList &source_indexes);

  // Always add new columns to the end of this enum - the values are persisted
  enum Column {
    Column_Title = 0,
    Column_Artist,
    Column_Album,
    Column_AlbumArtist,
    Column_Performer,
    Column_Composer,
    Column_Year,
    Column_OriginalYear,
    Column_Track,
    Column_Disc,
    Column_Length,
    Column_Genre,
    Column_Samplerate,
    Column_Bitdepth,
    Column_Bitrate,
    Column_Filename,
    Column_BaseFilename,
    Column_Filesize,
    Column_Filetype,
    Column_DateCreated,
    Column_DateModified,
    Column_PlayCount,
    Column_SkipCount,
    Column_LastPlayed,
    Column_Comment,
    Column_Grouping,
    Column_Source,
    Column_Mood,
    Column_Rating,
    Column_HasCUE,
    ColumnCount
  };

  enum Role {
    Role_IsCurrent = Qt::UserRole + 1,
    Role_IsPaused,
    Role_StopAfter,
    Role_QueuePosition,
    Role_CanSetRating,
  };

  enum class AutoScroll {
    Never,
    Maybe,
    Always
  };

  static const char *kCddaMimeType;
  static const char *kRowsMimetype;
  static const char *kPlayNowMimetype;

  static const int kInvalidSongPriority;
  static const QRgb kInvalidSongColor;

  static const int kDynamicHistoryPriority;
  static const QRgb kDynamicHistoryColor;

  static const char *kSettingsGroup;

  static const int kUndoStackSize;
  static const int kUndoItemLimit;

  static const qint64 kMinScrobblePointNsecs;
  static const qint64 kMaxScrobblePointNsecs;

  static bool CompareItems(const int column, const Qt::SortOrder order, PlaylistItemPtr a, PlaylistItemPtr b);

  static QString column_name(Column column);
  static QString abbreviated_column_name(Column column);

  static bool column_is_editable(Playlist::Column column);
  static bool set_column_value(Song &song, Column column, const QVariant &value);

  // Persistence
  void Restore();
  void ScheduleSaveAsync();

  // Accessors
  PlaylistFilter *filter() const;
  Queue *queue() const { return queue_; }

  int id() const { return id_; }
  const QString &ui_path() const { return ui_path_; }
  void set_ui_path(const QString &path) { ui_path_ = path; }
  bool is_favorite() const { return favorite_; }
  void set_favorite(const bool favorite) { favorite_ = favorite; }

  int current_row() const;
  int last_played_row() const;
  void reset_last_played() { last_played_item_index_ = QPersistentModelIndex(); }
  int next_row(const bool ignore_repeat_track = false) const;
  int previous_row(const bool ignore_repeat_track = false) const;

  const QModelIndex current_index() const;

  bool stop_after_current() const;
  bool is_dynamic() const { return static_cast<bool>(dynamic_playlist_); }
  int dynamic_history_length() const;

  QString special_type() const { return special_type_; }
  void set_special_type(const QString &v) { special_type_ = v; }

  const PlaylistItemPtr &item_at(const int index) const { return items_[index]; }
  bool has_item_at(const int index) const { return index >= 0 && index < rowCount(); }

  PlaylistItemPtr current_item() const;

  PlaylistItem::Options current_item_options() const;
  Song current_item_metadata() const;

  PlaylistItemPtrList collection_items_by_id(const int id) const;

  SongList GetAllSongs() const;
  PlaylistItemPtrList GetAllItems() const;
  quint64 GetTotalLength() const;  // in seconds

  void set_sequence(PlaylistSequence *v);
  PlaylistSequence *sequence() const { return playlist_sequence_; }

  QUndoStack *undo_stack() const { return undo_stack_; }

  bool scrobbled() const { return scrobbled_; }
  void set_scrobbled(const bool state) { scrobbled_ = state; }
  void set_editing(const int row) { editing_ = row; }
  qint64 scrobble_point_nanosec() const { return scrobble_point_; }
  void UpdateScrobblePoint(const qint64 seek_point_nanosec = 0);

  // Changing the playlist
  void InsertItems(const PlaylistItemPtrList &itemsIn, const int pos = -1, const bool play_now = false, const bool enqueue = false, const bool enqueue_next = false);
  void InsertCollectionItems(const SongList &songs, const int pos = -1, const bool play_now = false, const bool enqueue = false, const bool enqueue_next = false);
  void InsertSongs(const SongList &songs, const int pos = -1, const bool play_now = false, const bool enqueue = false, const bool enqueue_next = false);
  void InsertSongsOrCollectionItems(const SongList &songs, const int pos = -1, const bool play_now = false, const bool enqueue = false, const bool enqueue_next = false);
  void InsertSmartPlaylist(PlaylistGeneratorPtr gen, const int pos = -1, const bool play_now = false, const bool enqueue = false, const bool enqueue_next = false);
  void InsertInternetItems(InternetService *service, const SongList &songs, const int pos = -1, const bool play_now = false, const bool enqueue = false, const bool enqueue_next = false);
  void InsertRadioItems(const SongList &songs, const int pos = -1, const bool play_now = false, const bool enqueue = false, const bool enqueue_next = false);

  void ReshuffleIndices();

  // If this playlist contains the current item, this method will apply the "valid" flag on it.
  // If the "valid" flag is false, the song will be greyed out. Otherwise, the grey color will be undone.
  // If the song is a local file, and it's valid but non-existent or invalid but exists, the
  // song will be reloaded to even out the situation because obviously something has changed.
  // This returns true if this playlist had current item when the method was invoked.
  bool ApplyValidityOnCurrentSong(const QUrl &url, bool valid);
  // Grays out and reloads all deleted songs in all playlists. Also, "ungreys" those songs which were once deleted but now got restored somehow.
  void InvalidateDeletedSongs();
  // Removes from the playlist all local files that don't exist anymore.
  void RemoveDeletedSongs();

  void StopAfter(const int row);
  void ReloadItems(const QList<int> &rows);
  void ReloadItemsBlocking(const QList<int> &rows);
  void InformOfCurrentSongChange(const AutoScroll autoscroll, const bool minor);

  // Registers an object which will get notifications when new songs are about to be inserted into this playlist.
  void AddSongInsertVetoListener(SongInsertVetoListener *listener);
  // Unregisters a SongInsertVetoListener object.
  void RemoveSongInsertVetoListener(SongInsertVetoListener *listener);

  // Just emits the dataChanged() signal so the mood column is repainted.
#ifdef HAVE_MOODBAR
  void MoodbarUpdated(const QModelIndex &idx);
#endif

  // QAbstractListModel
  int rowCount(const QModelIndex& = QModelIndex()) const override { return items_.count(); }
  int columnCount(const QModelIndex& = QModelIndex()) const override { return ColumnCount; }
  QVariant data(const QModelIndex &idx, int role = Qt::DisplayRole) const override;
  bool setData(const QModelIndex &idx, const QVariant &value, int role) override;
  QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
  Qt::ItemFlags flags(const QModelIndex &idx) const override;
  QStringList mimeTypes() const override;
  Qt::DropActions supportedDropActions() const override;
  QMimeData *mimeData(const QModelIndexList &indexes) const override;
  bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int column, const QModelIndex &parent) override;
  void sort(int column, Qt::SortOrder order) override;
  bool removeRows(int row, int count, const QModelIndex &parent = QModelIndex()) override;

  static bool ComparePathDepths(Qt::SortOrder, PlaylistItemPtr, PlaylistItemPtr);

  void ItemChanged(PlaylistItemPtr item);
  void ItemChanged(const int row);

  // Changes rating of a song to the given value asynchronously
  void RateSong(const QModelIndex &idx, const float rating);
  void RateSongs(const QModelIndexList &index_list, const float rating);

  void set_auto_sort(const bool auto_sort) { auto_sort_ = auto_sort; }

  void ItemReload(const QPersistentModelIndex &idx, const Song &old_metadata, const bool metadata_edit);

 public slots:
  void set_current_row(const int i, const Playlist::AutoScroll autoscroll = Playlist::AutoScroll::Maybe, const bool is_stopping = false, const bool force_inform = false);
  void Paused();
  void Playing();
  void Stopped();
  void IgnoreSorting(const bool value) { ignore_sorting_ = value; }

  void ClearStreamMetadata();
  void SetStreamMetadata(const QUrl &url, const Song &song, const bool minor);
  void UpdateItems(SongList songs);

  void Clear();
  void RemoveDuplicateSongs();
  void RemoveUnavailableSongs();
  void Shuffle();

  void ShuffleModeChanged(const PlaylistSequence::ShuffleMode mode);

  void SetColumnAlignment(const ColumnAlignmentMap &alignment);

  void InsertUrls(const QList<QUrl> &urls, int pos = -1, bool play_now = false, bool enqueue = false, bool enqueue_next = false);
  // Removes items with given indices from the playlist. This operation is not undoable.
  void RemoveItemsWithoutUndo(const QList<int> &indicesIn);

  void ExpandDynamicPlaylist();
  void RepopulateDynamicPlaylist();
  void TurnOffDynamicPlaylist();

  void AlbumCoverLoaded(const Song &song, AlbumCoverLoaderResultPtr result);

 signals:
  void RestoreFinished();
  void PlaylistLoaded();
  void CurrentSongChanged(const Song &metadata);
  void SongMetadataChanged(const Song &metadata);
  void EditingFinished(const int playlist_id, const QModelIndex idx);
  void PlayRequested(const QModelIndex idx, const Playlist::AutoScroll autoscroll);
  void MaybeAutoscroll(const Playlist::AutoScroll autoscroll);

  // Signals that the underlying list of items was changed, meaning that something was added to it, removed from it or the ordering changed.
  void PlaylistChanged();
  void DynamicModeChanged(bool dynamic);

  void Error(QString message);

  // Signals that the queue has changed, meaning that the remaining queued items should update their position.
  void QueueChanged();

 private:
  void SetCurrentIsPaused(const bool paused);
  int NextVirtualIndex(int i, const bool ignore_repeat_track) const;
  int PreviousVirtualIndex(int i, const bool ignore_repeat_track) const;
  bool FilterContainsVirtualIndex(const int i) const;

  template<typename T>
  void InsertSongItems(const SongList &songs, const int pos, const bool play_now, const bool enqueue, const bool enqueue_next = false);

  // Modify the playlist without changing the undo stack.  These are used by our friends in PlaylistUndoCommands
  void InsertItemsWithoutUndo(const PlaylistItemPtrList &items, int pos, bool enqueue = false, bool enqueue_next = false);
  PlaylistItemPtrList RemoveItemsWithoutUndo(const int row, const int count);
  void MoveItemsWithoutUndo(const QList<int> &source_rows, int pos);
  void MoveItemWithoutUndo(const int source, const int dest);
  void MoveItemsWithoutUndo(int start, const QList<int> &dest_rows);
  void ReOrderWithoutUndo(const PlaylistItemPtrList &new_items);

  void RemoveItemsNotInQueue();

  // Removes rows with given indices from this playlist.
  bool removeRows(QList<int> &rows);

  void TurnOnDynamicPlaylist(PlaylistGeneratorPtr gen);
  void InsertDynamicItems(const int count);

 private slots:
  void TracksAboutToBeDequeued(const QModelIndex&, const int begin, const int end);
  void TracksDequeued();
  void TracksEnqueued(const QModelIndex&, const int begin, const int end);
  void QueueLayoutChanged();
  void SongSaveComplete(TagReaderReply *reply, const QPersistentModelIndex &idx, const Song &old_metadata);
  void ItemReloadComplete(const QPersistentModelIndex &idx, const Song &old_metadata, const bool metadata_edit);
  void ItemsLoaded();
  void SongInsertVetoListenerDestroyed();
  void ScheduleSave();
  void Save();

 private:
  bool is_loading_;
  PlaylistFilter *filter_;
  Queue *queue_;
  QTimer *timer_save_;

  QList<QModelIndex> temp_dequeue_change_indexes_;

  PlaylistBackend *backend_;
  TaskManager *task_manager_;
  CollectionBackend *collection_;
  int id_;
  QString ui_path_;
  bool favorite_;

  PlaylistItemPtrList items_;

  // Contains the indices into items_ in the order that they will be played.
  QList<int> virtual_items_;

  // A map of collection ID to playlist item - for fast lookups when collection items change.
  QMultiMap<int, PlaylistItemPtr> collection_items_by_id_;

  QPersistentModelIndex current_item_index_;
  QPersistentModelIndex last_played_item_index_;
  QPersistentModelIndex stop_after_;
  bool current_is_paused_;
  int current_virtual_index_;

  bool is_shuffled_;

  PlaylistSequence *playlist_sequence_;

  // Hack to stop QTreeView::setModel sorting the playlist
  bool ignore_sorting_;

  QUndoStack *undo_stack_;

  ColumnAlignmentMap column_alignments_;

  QList<SongInsertVetoListener*> veto_listeners_;

  QString special_type_;

  // Cancel async restore if songs are already replaced
  bool cancel_restore_;

  bool scrobbled_;
  qint64 scrobble_point_;

  int editing_;

  PlaylistGeneratorPtr dynamic_playlist_;

  bool auto_sort_;
  int sort_column_;
  Qt::SortOrder sort_order_;

};

#endif  // PLAYLIST_H
