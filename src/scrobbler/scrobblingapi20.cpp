/*
 * Strawberry Music Player
 * Copyright 2018-2023, Jonas Kvinge <jonas@jkvinge.net>
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

#include "config.h"

#include <algorithm>

#include <QApplication>
#include <QDesktopServices>
#include <QLocale>
#include <QClipboard>
#include <QPair>
#include <QVariant>
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <QDateTime>
#include <QTimer>
#include <QCryptographicHash>
#include <QMessageBox>
#include <QSettings>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QFlags>

#include "core/networkaccessmanager.h"
#include "core/song.h"
#include "core/logging.h"
#include "utilities/timeconstants.h"
#include "internet/localredirectserver.h"
#include "settings/scrobblersettingspage.h"

#include "audioscrobbler.h"
#include "scrobblerservice.h"
#include "scrobblingapi20.h"
#include "scrobblercache.h"
#include "scrobblercacheitem.h"
#include "scrobblemetadata.h"

const char *ScrobblingAPI20::kApiKey = "211990b4c96782c05d1536e7219eb56e";
const char *ScrobblingAPI20::kSecret = "80fd738f49596e9709b1bf9319c444a8";
const int ScrobblingAPI20::kScrobblesPerRequest = 50;

ScrobblingAPI20::ScrobblingAPI20(const QString &name, const QString &settings_group, const QString &auth_url, const QString &api_url, const bool batch, const QString &cache_file, AudioScrobbler *scrobbler, NetworkAccessManager *network, QObject *parent)
    : ScrobblerService(name, parent),
      name_(name),
      settings_group_(settings_group),
      auth_url_(auth_url),
      api_url_(api_url),
      batch_(batch),
      scrobbler_(scrobbler),
      network_(network),
      cache_(new ScrobblerCache(cache_file, this)),
      server_(nullptr),
      enabled_(false),
      https_(false),
      prefer_albumartist_(false),
      subscriber_(false),
      submitted_(false),
      scrobbled_(false),
      timestamp_(0),
      submit_error_(false) {

  timer_submit_.setSingleShot(true);
  QObject::connect(&timer_submit_, &QTimer::timeout, this, &ScrobblingAPI20::Submit);

  ReloadSettings();
  LoadSession();

}

ScrobblingAPI20::~ScrobblingAPI20() {

  while (!replies_.isEmpty()) {
    QNetworkReply *reply = replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
  }

  if (server_) {
    QObject::disconnect(server_, nullptr, this, nullptr);
    if (server_->isListening()) server_->close();
    server_->deleteLater();
  }

}

void ScrobblingAPI20::ReloadSettings() {

  QSettings s;

  s.beginGroup(settings_group_);
  enabled_ = s.value("enabled", false).toBool();
  https_ = s.value("https", false).toBool();
  s.endGroup();

  s.beginGroup(ScrobblerSettingsPage::kSettingsGroup);
  prefer_albumartist_ = s.value("albumartist", false).toBool();
  s.endGroup();

}

void ScrobblingAPI20::LoadSession() {

  QSettings s;
  s.beginGroup(settings_group_);
  subscriber_ = s.value("subscriber", false).toBool();
  username_ = s.value("username").toString();
  session_key_ = s.value("session_key").toString();
  s.endGroup();

}

void ScrobblingAPI20::Logout() {

  subscriber_ = false;
  username_.clear();
  session_key_.clear();

  QSettings settings;
  settings.beginGroup(settings_group_);
  settings.remove("subscriber");
  settings.remove("username");
  settings.remove("session_key");
  settings.endGroup();

}

ScrobblingAPI20::ReplyResult ScrobblingAPI20::GetJsonObject(QNetworkReply *reply, QJsonObject &json_obj, QString &error_description) {

  ReplyResult reply_error_type = ReplyResult::ServerError;

  if (reply->error() == QNetworkReply::NoError) {
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
      reply_error_type = ReplyResult::Success;
    }
    else {
      error_description = QString("Received HTTP code %1").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
    }
  }
  else {
    error_description = QString("%1 (%2)").arg(reply->errorString()).arg(reply->error());
  }

  // See if there is Json data containing "error" and "message" - then use that instead.
  if (reply->error() == QNetworkReply::NoError || reply->error() >= 200) {
    const QByteArray data = reply->readAll();
    int error_code = 0;
    if (!data.isEmpty() && ExtractJsonObj(data, json_obj, error_description) && json_obj.contains("error") && json_obj.contains("message")) {
      error_code = json_obj["error"].toInt();
      QString error_message = json_obj["message"].toString();
      error_description = QString("%1 (%2)").arg(error_message).arg(error_code);
      reply_error_type = ReplyResult::APIError;
    }
    const ScrobbleErrorCode lastfm_error_code = static_cast<ScrobbleErrorCode>(error_code);
    if (reply->error() == QNetworkReply::ContentAccessDenied ||
        reply->error() == QNetworkReply::ContentOperationNotPermittedError ||
        reply->error() == QNetworkReply::AuthenticationRequiredError ||
        lastfm_error_code == ScrobbleErrorCode::InvalidSessionKey ||
        lastfm_error_code == ScrobbleErrorCode::UnauthorizedToken ||
        lastfm_error_code == ScrobbleErrorCode::LoginRequired ||
        lastfm_error_code == ScrobbleErrorCode::AuthenticationFailed ||
        lastfm_error_code == ScrobbleErrorCode::APIKeySuspended
    ) {
      // Session is probably expired
      Logout();
    }
  }

  return reply_error_type;

}

void ScrobblingAPI20::Authenticate(const bool https) {

  if (!server_) {
    server_ = new LocalRedirectServer(this);
    server_->set_https(https);
    if (!server_->Listen()) {
      AuthError(server_->error());
      delete server_;
      server_ = nullptr;
      return;
    }
    QObject::connect(server_, &LocalRedirectServer::Finished, this, &ScrobblingAPI20::RedirectArrived);
  }

  QUrlQuery url_query;
  url_query.addQueryItem("api_key", kApiKey);
  url_query.addQueryItem("cb", server_->url().toString());
  QUrl url(auth_url_);
  url.setQuery(url_query);

  QMessageBox messagebox(QMessageBox::Information, tr("%1 Scrobbler Authentication").arg(name_), tr("Open URL in web browser?") + QString("<br /><a href=\"%1\">%1</a><br />").arg(url.toString()) + tr("Press \"Save\" to copy the URL to clipboard and manually open it in a web browser."), QMessageBox::Open|QMessageBox::Save|QMessageBox::Cancel);
  messagebox.setTextFormat(Qt::RichText);
  int result = messagebox.exec();
  switch (result) {
    case QMessageBox::Open: {
      bool openurl_result = QDesktopServices::openUrl(url);
      if (openurl_result) {
        break;
      }
      QMessageBox messagebox_error(QMessageBox::Warning, tr("%1 Scrobbler Authentication").arg(name_), tr("Could not open URL. Please open this URL in your browser") + QString(":<br /><a href=\"%1\">%1</a>").arg(url.toString()), QMessageBox::Ok);
      messagebox_error.setTextFormat(Qt::RichText);
      messagebox_error.exec();
    }
      [[fallthrough]];
    case QMessageBox::Save:
      QApplication::clipboard()->setText(url.toString());
      break;
    case QMessageBox::Cancel:
      if (server_) {
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
      }
      emit AuthenticationComplete(false);
      break;
    default:
      break;
  }
}

void ScrobblingAPI20::RedirectArrived() {

  if (!server_) return;

  if (server_->error().isEmpty()) {
    QUrl url = server_->request_url();
    if (url.isValid()) {
      QUrlQuery url_query(url);
      if (url_query.hasQueryItem("token")) {
        QString token = url_query.queryItemValue("token").toUtf8();
        RequestSession(token);
      }
      else {
        AuthError(tr("Invalid reply from web browser. Missing token."));
      }
    }
    else {
      AuthError(tr("Received invalid reply from web browser. Try the HTTPS option, or use another browser like Chromium or Chrome."));
    }
  }
  else {
    AuthError(server_->error());
  }

  server_->close();
  server_->deleteLater();
  server_ = nullptr;

}

void ScrobblingAPI20::RequestSession(const QString &token) {

  QUrl session_url(api_url_);
  QUrlQuery session_url_query;
  session_url_query.addQueryItem("api_key", kApiKey);
  session_url_query.addQueryItem("method", "auth.getSession");
  session_url_query.addQueryItem("token", token);
  QString data_to_sign;
  for (const Param &param : session_url_query.queryItems()) {
    data_to_sign += param.first + param.second;
  }
  data_to_sign += kSecret;
  QByteArray const digest = QCryptographicHash::hash(data_to_sign.toUtf8(), QCryptographicHash::Md5);
  QString signature = QString::fromLatin1(digest.toHex()).rightJustified(32, '0').toLower();
  session_url_query.addQueryItem("api_sig", signature);
  session_url_query.addQueryItem(QUrl::toPercentEncoding("format"), QUrl::toPercentEncoding("json"));
  session_url.setQuery(session_url_query);

  QNetworkRequest req(session_url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  QNetworkReply *reply = network_->get(req);
  replies_ << reply;
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() { AuthenticateReplyFinished(reply); });

}

void ScrobblingAPI20::AuthenticateReplyFinished(QNetworkReply *reply) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QJsonObject json_obj;
  QString error_message;
  if (GetJsonObject(reply, json_obj, error_message) != ReplyResult::Success) {
    AuthError(error_message);
    return;
  }

  if (!json_obj.contains("session")) {
    AuthError("Json reply from server is missing session.");
    return;
  }

  QJsonValue json_session = json_obj["session"];
  if (!json_session.isObject()) {
    AuthError("Json session is not an object.");
    return;
  }
  json_obj = json_session.toObject();
  if (json_obj.isEmpty()) {
    AuthError("Json session object is empty.");
    return;
  }
  if (!json_obj.contains("subscriber") || !json_obj.contains("name") || !json_obj.contains("key")) {
    AuthError("Json session object is missing values.");
    return;
  }

  subscriber_ = json_obj["subscriber"].toBool();
  username_ = json_obj["name"].toString();
  session_key_ = json_obj["key"].toString();

  QSettings s;
  s.beginGroup(settings_group_);
  s.setValue("subscriber", subscriber_);
  s.setValue("username", username_);
  s.setValue("session_key", session_key_);
  s.endGroup();

  emit AuthenticationComplete(true);

  StartSubmit();

}

QNetworkReply *ScrobblingAPI20::CreateRequest(const ParamList &request_params) {

  ParamList params = ParamList()
    << Param("api_key", kApiKey)
    << Param("sk", session_key_)
    << Param("lang", QLocale().name().left(2).toLower())
    << request_params;

  std::sort(params.begin(), params.end());

  QUrlQuery url_query;
  QString data_to_sign;
  for (const Param &param : params) {
    EncodedParam encoded_param(QUrl::toPercentEncoding(param.first), QUrl::toPercentEncoding(param.second));
    url_query.addQueryItem(encoded_param.first, encoded_param.second);
    data_to_sign += param.first + param.second;
  }
  data_to_sign += kSecret;

  QByteArray const digest = QCryptographicHash::hash(data_to_sign.toUtf8(), QCryptographicHash::Md5);
  QString signature = QString::fromLatin1(digest.toHex()).rightJustified(32, '0').toLower();

  url_query.addQueryItem("api_sig", QUrl::toPercentEncoding(signature));
  url_query.addQueryItem("format", QUrl::toPercentEncoding("json"));

  QUrl url(api_url_);
  QNetworkRequest req(url);
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
  QByteArray query = url_query.toString(QUrl::FullyEncoded).toUtf8();
  QNetworkReply *reply = network_->post(req, query);
  replies_ << reply;

  //qLog(Debug) << name_ << "Sending request" << url_query.toString(QUrl::FullyDecoded);

  return reply;

}

void ScrobblingAPI20::UpdateNowPlaying(const Song &song) {

  CheckScrobblePrevSong();

  song_playing_ = song;
  timestamp_ = QDateTime::currentDateTime().toSecsSinceEpoch();
  scrobbled_ = false;

  if (!IsAuthenticated() || !song.is_metadata_good() || scrobbler_->IsOffline()) return;

  ParamList params = ParamList()
    << Param("method", "track.updateNowPlaying")
    << Param("artist", prefer_albumartist_ ? song.effective_albumartist() : song.artist())
    << Param("track", StripTitle(song.title()));

  if (!song.album().isEmpty()) {
    params << Param("album", StripAlbum(song.album()));
  }

  if (!prefer_albumartist_ && !song.albumartist().isEmpty()) {
    params << Param("albumArtist", song.albumartist());
  }

  QNetworkReply *reply = CreateRequest(params);
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() { UpdateNowPlayingRequestFinished(reply); });

}

void ScrobblingAPI20::UpdateNowPlayingRequestFinished(QNetworkReply *reply) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QJsonObject json_obj;
  QString error_message;
  if (GetJsonObject(reply, json_obj, error_message) != ReplyResult::Success) {
    Error(error_message);
    return;
  }

  if (!json_obj.contains("nowplaying")) {
    Error("Json reply from server is missing nowplaying.", json_obj);
    return;
  }

}

void ScrobblingAPI20::ClearPlaying() {

  CheckScrobblePrevSong();

  song_playing_ = Song();
  scrobbled_ = false;
  timestamp_ = 0;

}

void ScrobblingAPI20::Scrobble(const Song &song) {

  if (song.id() != song_playing_.id() || song.url() != song_playing_.url() || !song.is_metadata_good()) return;

  scrobbled_ = true;

  cache_->Add(song, timestamp_);

  if (scrobbler_->IsOffline()) return;

  if (!IsAuthenticated()) {
    if (scrobbler_->ShowErrorDialog()) {
      emit ErrorMessage(tr("Scrobbler %1 is not authenticated!").arg(name_));
    }
    return;
  }

  StartSubmit(true);
}

void ScrobblingAPI20::StartSubmit(const bool initial) {

  if (!submitted_ && cache_->Count() > 0) {
    if (initial && (!batch_ || scrobbler_->SubmitDelay() <= 0) && !submit_error_) {
      if (timer_submit_.isActive()) {
        timer_submit_.stop();
      }
      Submit();
    }
    else if (!timer_submit_.isActive()) {
      int submit_delay = static_cast<int>(std::max(scrobbler_->SubmitDelay(), submit_error_ ? 30 : 5) * kMsecPerSec);
      timer_submit_.setInterval(submit_delay);
      timer_submit_.start();
    }
  }

}

void ScrobblingAPI20::Submit() {

  if (!IsEnabled() || !IsAuthenticated() || scrobbler_->IsOffline()) return;

  qLog(Debug) << name_ << "Submitting scrobbles.";

  ParamList params = ParamList() << Param("method", "track.scrobble");

  int i = 0;
  ScrobblerCacheItemPtrList all_cache_items = cache_->List();
  ScrobblerCacheItemPtrList cache_items_sent;
  for (ScrobblerCacheItemPtr cache_item : all_cache_items) {
    if (cache_item->sent) continue;
    cache_item->sent = true;
    if (!batch_) {
      SendSingleScrobble(cache_item);
      continue;
    }
    cache_items_sent << cache_item;
    params << Param(QString("%1[%2]").arg("artist").arg(i), prefer_albumartist_ ? cache_item->metadata.effective_albumartist() : cache_item->metadata.artist);
    params << Param(QString("%1[%2]").arg("track").arg(i), StripTitle(cache_item->metadata.title));
    params << Param(QString("%1[%2]").arg("timestamp").arg(i), QString::number(cache_item->timestamp));
    params << Param(QString("%1[%2]").arg("duration").arg(i), QString::number(cache_item->metadata.length_nanosec / kNsecPerSec));
    if (!cache_item->metadata.album.isEmpty()) {
      params << Param(QString("%1[%2]").arg("album").arg(i), StripAlbum(cache_item->metadata.album));
    }
    if (!prefer_albumartist_ && !cache_item->metadata.albumartist.isEmpty()) {
      params << Param(QString("%1[%2]").arg("albumArtist").arg(i), cache_item->metadata.albumartist);
    }
    if (cache_item->metadata.track > 0) {
      params << Param(QString("%1[%2]").arg("trackNumber").arg(i), QString::number(cache_item->metadata.track));
    }
    ++i;
    if (cache_items_sent.count() >= kScrobblesPerRequest) break;
  }

  if (!batch_ || cache_items_sent.count() <= 0) return;

  submitted_ = true;

  QNetworkReply *reply = CreateRequest(params);
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, cache_items_sent]() { ScrobbleRequestFinished(reply, cache_items_sent); });

}

void ScrobblingAPI20::ScrobbleRequestFinished(QNetworkReply *reply, ScrobblerCacheItemPtrList cache_items) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  submitted_ = false;

  QJsonObject json_obj;
  QString error_message;
  if (GetJsonObject(reply, json_obj, error_message) != ReplyResult::Success) {
    Error(error_message);
    cache_->ClearSent(cache_items);
    submit_error_ = true;
    StartSubmit();
    return;
  }

  cache_->Flush(cache_items);
  submit_error_ = false;

  if (!json_obj.contains("scrobbles")) {
    Error("Json reply from server is missing scrobbles.", json_obj);
    StartSubmit();
    return;
  }

  QJsonValue value_scrobbles = json_obj["scrobbles"];
  if (!value_scrobbles.isObject()) {
    Error("Json scrobbles is not an object.", json_obj);
    StartSubmit();
    return;
  }
  json_obj = value_scrobbles.toObject();
  if (json_obj.isEmpty()) {
    Error("Json scrobbles object is empty.", value_scrobbles);
    StartSubmit();
    return;
  }
  if (!json_obj.contains("@attr") || !json_obj.contains("scrobble")) {
    Error("Json scrobbles object is missing values.", json_obj);
    StartSubmit();
    return;
  }

  QJsonValue value_attr = json_obj["@attr"];
  if (!value_attr.isObject()) {
    Error("Json scrobbles attr is not an object.", value_attr);
    StartSubmit();
    return;
  }
  QJsonObject obj_attr = value_attr.toObject();
  if (obj_attr.isEmpty()) {
    Error("Json scrobbles attr is empty.", value_attr);
    StartSubmit();
    return;
  }
  if (!obj_attr.contains("accepted") || !obj_attr.contains("ignored")) {
    Error("Json scrobbles attr is missing values.", obj_attr);
    StartSubmit();
    return;
  }
  int accepted = obj_attr["accepted"].toInt();
  int ignored = obj_attr["ignored"].toInt();

  qLog(Debug) << name_ << "Scrobbles accepted:" << accepted << "ignored:" << ignored;

  QJsonArray array_scrobble;

  QJsonValue value_scrobble = json_obj["scrobble"];
  if (value_scrobble.isObject()) {
    QJsonObject obj_scrobble = value_scrobble.toObject();
    if (obj_scrobble.isEmpty()) {
      Error("Json scrobbles scrobble object is empty.", obj_scrobble);
      StartSubmit();
      return;
    }
    array_scrobble.append(obj_scrobble);
  }
  else if (value_scrobble.isArray()) {
    array_scrobble = value_scrobble.toArray();
    if (array_scrobble.isEmpty()) {
      Error("Json scrobbles scrobble array is empty.", value_scrobble);
      StartSubmit();
      return;
    }
  }
  else {
    Error("Json scrobbles scrobble is not an object or array.", value_scrobble);
    StartSubmit();
    return;
  }

  for (const QJsonValueRef value : array_scrobble) {

    if (!value.isObject()) {
      Error("Json scrobbles scrobble array value is not an object.");
      continue;
    }
    QJsonObject json_track = value.toObject();
    if (json_track.isEmpty()) {
      continue;
    }

    if (!json_track.contains("artist") ||
        !json_track.contains("album") ||
        !json_track.contains("albumArtist") ||
        !json_track.contains("track") ||
        !json_track.contains("timestamp") ||
        !json_track.contains("ignoredMessage")
    ) {
      Error("Json scrobbles scrobble is missing values.", json_track);
      continue;
    }

    QJsonValue value_artist = json_track["artist"];
    QJsonValue value_album = json_track["album"];
    QJsonValue value_song = json_track["track"];
    QJsonValue value_ignoredmessage = json_track["ignoredMessage"];
    //quint64 timestamp = json_track["timestamp"].toVariant().toULongLong();

    if (!value_artist.isObject() || !value_album.isObject() || !value_song.isObject() || !value_ignoredmessage.isObject()) {
      Error("Json scrobbles scrobble values are not objects.", json_track);
      continue;
    }

    QJsonObject obj_artist = value_artist.toObject();
    QJsonObject obj_album = value_album.toObject();
    QJsonObject obj_song = value_song.toObject();
    QJsonObject obj_ignoredmessage = value_ignoredmessage.toObject();

    if (obj_artist.isEmpty() || obj_album.isEmpty() || obj_song.isEmpty() || obj_ignoredmessage.isEmpty()) {
      Error("Json scrobbles scrobble values objects are empty.", json_track);
      continue;
    }

    if (!obj_artist.contains("#text") || !obj_album.contains("#text") || !obj_song.contains("#text")) {
      continue;
    }

    //QString artist = obj_artist["#text"].toString();
    //QString album = obj_album["#text"].toString();
    QString song = obj_song["#text"].toString();
    bool ignoredmessage = obj_ignoredmessage["code"].toVariant().toBool();
    QString ignoredmessage_text = obj_ignoredmessage["#text"].toString();

    if (ignoredmessage) {
      Error(QString("Scrobble for \"%1\" ignored: %2").arg(song, ignoredmessage_text));
    }
    else {
      qLog(Debug) << name_ << "Scrobble for" << song << "accepted";
    }

 }

  StartSubmit();

}

void ScrobblingAPI20::SendSingleScrobble(ScrobblerCacheItemPtr item) {

  ParamList params = ParamList()
    << Param("method", "track.scrobble")
    << Param("artist", prefer_albumartist_ ? item->metadata.effective_albumartist() : item->metadata.artist)
    << Param("track", StripTitle(item->metadata.title))
    << Param("timestamp", QString::number(item->timestamp))
    << Param("duration", QString::number(item->metadata.length_nanosec / kNsecPerSec));

  if (!item->metadata.album.isEmpty()) {
    params << Param("album", StripAlbum(item->metadata.album));
  }
  if (!prefer_albumartist_ && !item->metadata.albumartist.isEmpty()) {
    params << Param("albumArtist", item->metadata.albumartist);
  }
  if (item->metadata.track > 0) {
    params << Param("trackNumber", QString::number(item->metadata.track));
  }

  QNetworkReply *reply = CreateRequest(params);
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, item]() { SingleScrobbleRequestFinished(reply, item); });

}

void ScrobblingAPI20::SingleScrobbleRequestFinished(QNetworkReply *reply, ScrobblerCacheItemPtr cache_item) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QJsonObject json_obj;
  QString error_message;
  if (GetJsonObject(reply, json_obj, error_message) != ReplyResult::Success) {
    Error(error_message);
    cache_item->sent = false;
    return;
  }

  if (!json_obj.contains("scrobbles")) {
    Error("Json reply from server is missing scrobbles.", json_obj);
    cache_item->sent = false;
    return;
  }

  cache_->Remove(cache_item);

  QJsonValue value_scrobbles = json_obj["scrobbles"];
  if (!value_scrobbles.isObject()) {
    Error("Json scrobbles is not an object.", json_obj);
    return;
  }
  json_obj = value_scrobbles.toObject();
  if (json_obj.isEmpty()) {
    Error("Json scrobbles object is empty.", value_scrobbles);
    return;
  }
  if (!json_obj.contains("@attr") || !json_obj.contains("scrobble")) {
    Error("Json scrobbles object is missing values.", json_obj);
    return;
  }

  QJsonValue value_attr = json_obj["@attr"];
  if (!value_attr.isObject()) {
    Error("Json scrobbles attr is not an object.", value_attr);
    return;
  }
  QJsonObject obj_attr = value_attr.toObject();
  if (obj_attr.isEmpty()) {
    Error("Json scrobbles attr is empty.", value_attr);
    return;
  }

  QJsonValue value_scrobble = json_obj["scrobble"];
  if (!value_scrobble.isObject()) {
    Error("Json scrobbles scrobble is not an object.", value_scrobble);
    return;
  }
  QJsonObject json_obj_scrobble = value_scrobble.toObject();
  if (json_obj_scrobble.isEmpty()) {
    Error("Json scrobbles scrobble is empty.", value_scrobble);
    return;
  }

  if (!obj_attr.contains("accepted") || !obj_attr.contains("ignored")) {
    Error("Json scrobbles attr is missing values.", obj_attr);
    return;
  }

  if (!json_obj_scrobble.contains("artist") || !json_obj_scrobble.contains("album") || !json_obj_scrobble.contains("albumArtist") || !json_obj_scrobble.contains("track") || !json_obj_scrobble.contains("timestamp")) {
    Error("Json scrobbles scrobble is missing values.", json_obj_scrobble);
    return;
  }

  QJsonValue json_value_artist = json_obj_scrobble["artist"];
  QJsonValue json_value_album = json_obj_scrobble["album"];
  QJsonValue json_value_song = json_obj_scrobble["track"];

  if (!json_value_artist.isObject() || !json_value_album.isObject() || !json_value_song.isObject()) {
    Error("Json scrobbles scrobble values are not objects.", json_obj_scrobble);
    return;
  }

  QJsonObject json_obj_artist = json_value_artist.toObject();
  QJsonObject json_obj_album = json_value_album.toObject();
  QJsonObject json_obj_song = json_value_song.toObject();

  if (json_obj_artist.isEmpty() || json_obj_album.isEmpty() || json_obj_song.isEmpty()) {
    Error("Json scrobbles scrobble values objects are empty.", json_obj_scrobble);
    return;
  }

  if (!json_obj_artist.contains("#text") || !json_obj_album.contains("#text") || !json_obj_song.contains("#text")) {
    Error("Json scrobbles scrobble values objects are missing #text.", json_obj_artist);
    return;
  }

  //QString artist = json_obj_artist["#text"].toString();
  //QString album = json_obj_album["#text"].toString();
  QString song = json_obj_song["#text"].toString();

  int accepted = obj_attr["accepted"].toVariant().toInt();
  if (accepted == 1) {
    qLog(Debug) << name_ << "Scrobble for" << song << "accepted";
  }
  else {
    Error(QString("Scrobble for \"%1\" not accepted").arg(song));
  }

}

void ScrobblingAPI20::Love() {

  if (!song_playing_.is_valid() || !song_playing_.is_metadata_good()) return;

  if (!IsAuthenticated()) scrobbler_->ShowConfig();

  qLog(Debug) << name_ << "Sending love for song" << song_playing_.artist() << song_playing_.album() << song_playing_.title();

  ParamList params = ParamList()
    << Param("method", "track.love")
    << Param("artist", prefer_albumartist_ ? song_playing_.effective_albumartist() : song_playing_.artist())
    << Param("track", song_playing_.title());

  if (!song_playing_.album().isEmpty()) {
    params << Param("album", song_playing_.album());
  }

  if (!prefer_albumartist_ && !song_playing_.albumartist().isEmpty()) {
    params << Param("albumArtist", song_playing_.albumartist());
  }

  QNetworkReply *reply = CreateRequest(params);
  QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] { LoveRequestFinished(reply); });

}

void ScrobblingAPI20::LoveRequestFinished(QNetworkReply *reply) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QJsonObject json_obj;
  QString error_message;
  if (GetJsonObject(reply, json_obj, error_message) != ReplyResult::Success) {
    Error(error_message);
    return;
  }

  if (json_obj.contains("error")) {
    QJsonValue json_value = json_obj["error"];
    if (!json_value.isObject()) {
      Error("Error is not on object.");
      return;
    }
    QJsonObject json_obj_error = json_value.toObject();
    if (json_obj_error.isEmpty()) {
      Error("Received empty json error object.", json_obj);
      return;
    }
    if (json_obj_error.contains("code") && json_obj_error.contains("#text")) {
      int code = json_obj_error["code"].toInt();
      QString text = json_obj_error["#text"].toString();
      QString error_reason = QString("%1 (%2)").arg(text).arg(code);
      Error(error_reason);
      return;
    }
  }

  if (json_obj.contains("lfm")) {
    QJsonValue json_value = json_obj["lfm"];
    if (json_value.isObject()) {
      QJsonObject json_obj_lfm = json_value.toObject();
      if (json_obj_lfm.contains("status")) {
        QString status = json_obj_lfm["status"].toString();
        qLog(Debug) << name_ << "Received love status:" << status;
        return;
      }
    }
  }

}

void ScrobblingAPI20::AuthError(const QString &error) {

  qLog(Error) << name_ << error;
  emit AuthenticationComplete(false, error);

}

void ScrobblingAPI20::Error(const QString &error, const QVariant &debug) {

  qLog(Error) << name_ << error;
  if (debug.isValid()) qLog(Debug) << debug;

  if (scrobbler_->ShowErrorDialog()) {
    emit ErrorMessage(tr("Scrobbler %1 error: %2").arg(name_, error));
  }
}

QString ScrobblingAPI20::ErrorString(const ScrobbleErrorCode error) {

  switch (error) {
    case ScrobbleErrorCode::NoError:
      return QString("This error does not exist.");
    case ScrobbleErrorCode::InvalidService:
      return QString("Invalid service - This service does not exist.");
    case ScrobbleErrorCode::InvalidMethod:
      return QString("Invalid Method - No method with that name in this package.");
    case ScrobbleErrorCode::AuthenticationFailed:
      return QString("Authentication Failed - You do not have permissions to access the service.");
    case ScrobbleErrorCode::InvalidFormat:
      return QString("Invalid format - This service doesn't exist in that format.");
    case ScrobbleErrorCode::InvalidParameters:
      return QString("Invalid parameters - Your request is missing a required parameter.");
    case ScrobbleErrorCode::InvalidResourceSpecified:
      return QString("Invalid resource specified");
    case ScrobbleErrorCode::OperationFailed:
      return QString("Operation failed - Most likely the backend service failed. Please try again.");
    case ScrobbleErrorCode::InvalidSessionKey:
      return QString("Invalid session key - Please re-authenticate.");
    case ScrobbleErrorCode::InvalidApiKey:
      return QString("Invalid API key - You must be granted a valid key by last.fm.");
    case ScrobbleErrorCode::ServiceOffline:
      return QString("Service Offline - This service is temporarily offline. Try again later.");
    case ScrobbleErrorCode::SubscribersOnly:
      return QString("Subscribers Only - This station is only available to paid last.fm subscribers.");
    case ScrobbleErrorCode::InvalidMethodSignature:
      return QString("Invalid method signature supplied.");
    case ScrobbleErrorCode::UnauthorizedToken:
      return QString("Unauthorized Token - This token has not been authorized.");
    case ScrobbleErrorCode::ItemUnavailable:
      return QString("This item is not available for streaming.");
    case ScrobbleErrorCode::TemporarilyUnavailable:
      return QString("The service is temporarily unavailable, please try again.");
    case ScrobbleErrorCode::LoginRequired:
      return QString("Login: User requires to be logged in.");
    case ScrobbleErrorCode::TrialExpired:
      return QString("Trial Expired - This user has no free radio plays left. Subscription required.");
    case ScrobbleErrorCode::ErrorDoesNotExist:
      return QString("This error does not exist.");
    case ScrobbleErrorCode::NotEnoughContent:
      return QString("Not Enough Content - There is not enough content to play this station.");
    case ScrobbleErrorCode::NotEnoughMembers:
      return QString("Not Enough Members - This group does not have enough members for radio.");
    case ScrobbleErrorCode::NotEnoughFans:
      return QString("Not Enough Fans - This artist does not have enough fans for for radio.");
    case ScrobbleErrorCode::NotEnoughNeighbours:
      return QString("Not Enough Neighbours - There are not enough neighbours for radio.");
    case ScrobbleErrorCode::NoPeakRadio:
      return QString("No Peak Radio - This user is not allowed to listen to radio during peak usage.");
    case ScrobbleErrorCode::RadioNotFound:
      return QString("Radio Not Found - Radio station not found.");
    case ScrobbleErrorCode::APIKeySuspended:
      return QString("Suspended API key - Access for your account has been suspended, please contact Last.fm");
    case ScrobbleErrorCode::Deprecated:
      return QString("Deprecated - This type of request is no longer supported.");
    case ScrobbleErrorCode::RateLimitExceeded:
      return QString("Rate limit exceeded - Your IP has made too many requests in a short period.");
  }

  return QString("Unknown error.");

}

void ScrobblingAPI20::CheckScrobblePrevSong() {

  qint64 duration = QDateTime::currentDateTime().toSecsSinceEpoch() - static_cast<qint64>(timestamp_);
  if (duration < 0) duration = 0;

  if (!scrobbled_ && song_playing_.is_metadata_good() && song_playing_.is_radio() && duration > 30) {
    Song song(song_playing_);
    song.set_length_nanosec(duration * kNsecPerSec);
    Scrobble(song);
  }

}
