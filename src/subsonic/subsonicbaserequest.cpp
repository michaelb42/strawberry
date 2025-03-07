/*
 * Strawberry Music Player
 * Copyright 2019-2021, Jonas Kvinge <jonas@jkvinge.net>
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

#include <memory>

#include <QtGlobal>
#include <QObject>
#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QCryptographicHash>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QSslError>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include "utilities/randutils.h"
#include "subsonicservice.h"
#include "subsonicbaserequest.h"

#include "settings/subsonicsettingspage.h"

SubsonicBaseRequest::SubsonicBaseRequest(SubsonicService *service, QObject *parent)
    : QObject(parent),
      service_(service),
      network_(new QNetworkAccessManager) {

  network_->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

}

QUrl SubsonicBaseRequest::CreateUrl(const QUrl &server_url, const SubsonicSettingsPage::AuthMethod auth_method, const QString &username, const QString &password, const QString &ressource_name, const ParamList &params_provided) {

  ParamList params = ParamList() << params_provided
                                 << Param("c", SubsonicService::kClientName)
                                 << Param("v", SubsonicService::kApiVersion)
                                 << Param("f", "json")
                                 << Param("u", username);

  if (auth_method == SubsonicSettingsPage::AuthMethod::Hex) {
    params << Param("p", QString("enc:" + password.toUtf8().toHex()));
  }
  else {
    const QString salt = Utilities::CryptographicRandomString(20);
    QCryptographicHash md5(QCryptographicHash::Md5);
    md5.addData(password.toUtf8());
    md5.addData(salt.toUtf8());
    params << Param("s", salt);
    params << Param("t", md5.result().toHex());
  }

  QUrlQuery url_query;
  for (const Param &param : params) {
    url_query.addQueryItem(QUrl::toPercentEncoding(param.first), QUrl::toPercentEncoding(param.second));
  }

  QUrl url(server_url);

  if (!url.path().isEmpty() && url.path().right(1) == "/") {
    url.setPath(url.path() + QString("rest/") + ressource_name + QString(".view"));
  }
  else {
    url.setPath(url.path() + QString("/rest/") + ressource_name + QString(".view"));
  }

  url.setQuery(url_query);

  return url;

}

QNetworkReply *SubsonicBaseRequest::CreateGetRequest(const QString &ressource_name, const ParamList &params_provided) const {

  QUrl url = CreateUrl(server_url(), auth_method(), username(), password(), ressource_name, params_provided);
  QNetworkRequest req(url);

  if (url.scheme() == "https" && !verify_certificate()) {
    QSslConfiguration sslconfig = QSslConfiguration::defaultConfiguration();
    sslconfig.setPeerVerifyMode(QSslSocket::VerifyNone);
    req.setSslConfiguration(sslconfig);
  }

  req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
  req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
  req.setAttribute(QNetworkRequest::Http2AllowedAttribute, http2());
#endif

  QNetworkReply *reply = network_->get(req);
  QObject::connect(reply, &QNetworkReply::sslErrors, this, &SubsonicBaseRequest::HandleSSLErrors);

  //qLog(Debug) << "Subsonic: Sending request" << url;

  return reply;

}

void SubsonicBaseRequest::HandleSSLErrors(const QList<QSslError> &ssl_errors) {

  for (const QSslError &ssl_error : ssl_errors) {
    Error(ssl_error.errorString());
  }

}

QByteArray SubsonicBaseRequest::GetReplyData(QNetworkReply *reply) {

  QByteArray data;

  if (reply->error() == QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
    data = reply->readAll();
  }
  else {
    if (reply->error() != QNetworkReply::NoError && reply->error() < 200) {
      // This is a network error, there is nothing more to do.
      Error(QString("%1 (%2)").arg(reply->errorString()).arg(reply->error()));
    }
    else {

      // See if there is Json data containing "error" - then use that instead.
      data = reply->readAll();
      QString error;
      QJsonParseError parse_error;
      QJsonDocument json_doc = QJsonDocument::fromJson(data, &parse_error);
      if (parse_error.error == QJsonParseError::NoError && !json_doc.isEmpty() && json_doc.isObject()) {
        QJsonObject json_obj = json_doc.object();
        if (!json_obj.isEmpty() && json_obj.contains("error")) {
          QJsonValue json_error = json_obj["error"];
          if (json_error.isObject()) {
            json_obj = json_error.toObject();
            if (!json_obj.isEmpty() && json_obj.contains("code") && json_obj.contains("message")) {
              int code = json_obj["code"].toInt();
              QString message = json_obj["message"].toString();
              error = QString("%1 (%2)").arg(message).arg(code);
            }
          }
        }
      }
      if (error.isEmpty()) {
        if (reply->error() != QNetworkReply::NoError) {
          error = QString("%1 (%2)").arg(reply->errorString()).arg(reply->error());
        }
        else {
          error = QString("Received HTTP code %1").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        }
      }
      Error(error);
    }
  }

  return data;

}

QJsonObject SubsonicBaseRequest::ExtractJsonObj(QByteArray &data) {

  QJsonParseError json_error;
  QJsonDocument json_doc = QJsonDocument::fromJson(data, &json_error);

  if (json_error.error != QJsonParseError::NoError) {
    Error("Reply from server missing Json data.", data);
    return QJsonObject();
  }

  if (json_doc.isEmpty()) {
    Error("Received empty Json document.", data);
    return QJsonObject();
  }

  if (!json_doc.isObject()) {
    Error("Json document is not an object.", json_doc);
    return QJsonObject();
  }

  QJsonObject json_obj = json_doc.object();
  if (json_obj.isEmpty()) {
    Error("Received empty Json object.", json_doc);
    return QJsonObject();
  }

  if (!json_obj.contains("subsonic-response")) {
    Error("Json reply is missing subsonic-response.", json_obj);
    return QJsonObject();
  }

  QJsonValue json_response = json_obj["subsonic-response"];
  if (!json_response.isObject()) {
    Error("Json response is not an object.", json_response);
    return QJsonObject();
  }
  json_obj = json_response.toObject();

  return json_obj;

}

QString SubsonicBaseRequest::ErrorsToHTML(const QStringList &errors) {

  QString error_html;
  for (const QString &error : errors) {
    error_html += error + "<br />";
  }
  return error_html;

}
