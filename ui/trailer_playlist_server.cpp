#include "trailer_playlist_server.h"

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>

namespace {

const QByteArray kPlaylistType = "Content-Type: application/vnd.apple.mpegurl\r\n";

QString absolute(const QUrl &base, const QString &uri) {
    return base.resolved(QUrl(uri)).toString(QUrl::FullyEncoded);
}

} // namespace

TrailerPlaylistServer::TrailerPlaylistServer(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent), m_network(network) {}

QString TrailerPlaylistServer::localUrlFor(const QString &master) {
    if (master.isEmpty() || !ensureListening())
        return master;

    int id = m_ids.value(master, 0);
    if (id == 0) {
        id = int(m_ids.size()) + 1;
        m_ids.insert(master, id);
        m_trailers[id].master = master;
    }
    return urlFor(id, 0, "master");
}

QString TrailerPlaylistServer::urlFor(int id, int chunk, const char *document) const {
    return QStringLiteral("http://127.0.0.1:%1/trailer/%2/%3/%4.m3u8")
        .arg(m_server->serverPort()).arg(id).arg(chunk).arg(QLatin1String(document));
}

QVariantMap TrailerPlaylistServer::seek(const QString &localUrl, qint64 positionMs) const {
    static const QRegularExpression ours(QStringLiteral("^http://127\\.0\\.0\\.1:\\d+/trailer/(\\d+)/"));
    const QRegularExpressionMatch match = ours.match(localUrl);
    if (!match.hasMatch() || !m_server)
        return {};
    const int id = match.captured(1).toInt();
    const auto trailer = m_trailers.constFind(id);
    if (trailer == m_trailers.constEnd() || !trailer->loaded)
        return {};

    const Media &video = trailer->video;
    const int chunk = chunkAt(video, qMax<qint64>(0, positionMs) / 1000.0);
    QVariantMap result;
    result["url"]        = urlFor(id, chunk, "master");
    result["offsetMs"]   = qRound64(startOf(video, chunk) * 1000.0);
    result["durationMs"] = qRound64(startOf(video, int(video.chunks.size())) * 1000.0);
    return result;
}

bool TrailerPlaylistServer::ensureListening() {
    if (m_server)
        return m_server->isListening();
    m_server = new QTcpServer(this);
    // Loopback only, on whatever port is free.
    if (!m_server->listen(QHostAddress::LocalHost, 0))
        return false;
    connect(m_server, &QTcpServer::newConnection, this, [this]() {
        while (QTcpSocket *socket = m_server->nextPendingConnection())
            handleConnection(socket);
    });
    return true;
}

void TrailerPlaylistServer::handleConnection(QTcpSocket *socket) {
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
        // One request per connection, and only its request line matters;
        // wait for the end of the headers so the reply does not race them.
        const QByteArray head = socket->peek(8192);
        if (!head.contains("\r\n\r\n")) {
            if (head.size() >= 8192)
                reply(socket, "400 Bad Request", {});
            return;
        }
        socket->readAll();
        disconnect(socket, &QTcpSocket::readyRead, this, nullptr);

        static const QRegularExpression request(
            QStringLiteral("^GET /trailer/(\\d+)/(\\d+)/(master|video|audio)\\.m3u8[ ?]"));
        const QRegularExpressionMatch match =
            request.match(QString::fromLatin1(head.left(head.indexOf("\r\n"))));
        const int id = match.hasMatch() ? match.captured(1).toInt() : 0;
        if (!m_trailers.contains(id)) {
            reply(socket, "404 Not Found", {});
            return;
        }
        serve(socket, id, match.captured(2).toInt(), match.captured(3));
    });
}

void TrailerPlaylistServer::serve(QTcpSocket *socket, int id, int chunk, const QString &document) {
    // Guards a player that gave up on the socket before Steam answered.
    QPointer<QTcpSocket> guarded(socket);
    load(id, [this, guarded, id, chunk, document](bool ok) {
        if (!guarded)
            return;
        const Trailer &trailer = m_trailers[id];
        if (!ok) {
            // The master falls back to Steam's own, which plays (without
            // seeking); the media playlists only exist once it has loaded.
            if (document == "master")
                reply(guarded, "302 Found", "Location: " + QUrl(trailer.master).toEncoded() + "\r\n");
            else
                reply(guarded, "502 Bad Gateway", {});
            return;
        }
        if (chunk >= trailer.video.chunks.size()) {
            reply(guarded, "404 Not Found", {});
            return;
        }

        QByteArray body;
        if (document == "master") {
            // Relative URIs, so they resolve against this same chunk's folder.
            QStringList lines = trailer.masterHead;
            if (!trailer.audioMedia.isEmpty())
                lines << QString(trailer.audioMedia).replace("%URI%", "audio.m3u8");
            lines << trailer.variantInf << "video.m3u8";
            body = (lines.join('\n') + '\n').toUtf8();
        } else if (document == "video") {
            body = renderMedia(trailer.video, chunk);
        } else {
            // The audio chunks need not line up with the video's; start at the
            // one holding the moment the video starts.
            const double start = startOf(trailer.video, chunk);
            body = renderMedia(trailer.audio, chunkAt(trailer.audio, start));
        }
        reply(guarded, "200 OK", kPlaylistType, body);
    });
}

// Reads the master and both media playlists once per trailer, then answers
// everyone who asked meanwhile. A failure is not remembered, so the next
// request tries again.
void TrailerPlaylistServer::load(int id, std::function<void(bool)> done) {
    Trailer &trailer = m_trailers[id];
    if (trailer.loaded) {
        done(true);
        return;
    }
    trailer.waiting << std::move(done);
    if (trailer.loading)
        return;
    trailer.loading = true;

    const QUrl masterUrl(trailer.master);
    fetch(masterUrl, [this, id](bool ok, const QByteArray &text) {
        QUrl videoUrl, audioUrl;
        if (!ok || !parseMaster(m_trailers[id], text, videoUrl, audioUrl)) {
            finishLoad(id, false);
            return;
        }
        fetch(videoUrl, [this, id, videoUrl, audioUrl](bool ok, const QByteArray &text) {
            if (!ok || !parseMedia(m_trailers[id].video, text, videoUrl)) {
                finishLoad(id, false);
                return;
            }
            if (audioUrl.isEmpty()) {
                finishLoad(id, true);
                return;
            }
            fetch(audioUrl, [this, id, audioUrl](bool ok, const QByteArray &text) {
                finishLoad(id, ok && parseMedia(m_trailers[id].audio, text, audioUrl));
            });
        });
    });
}

void TrailerPlaylistServer::finishLoad(int id, bool ok) {
    Trailer &trailer = m_trailers[id];
    trailer.loading = false;
    trailer.loaded = ok;
    const QList<std::function<void(bool)>> waiting = std::move(trailer.waiting);
    trailer.waiting.clear();
    for (const auto &done : waiting)
        done(ok);
}

void TrailerPlaylistServer::fetch(const QUrl &url, std::function<void(bool, QByteArray)> done) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "VortexLauncher/1.0");
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)]() {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError;
        done(ok, ok ? reply->readAll() : QByteArray());
    });
}

// Keeps the best video variant -- the highest BANDWIDTH -- and the audio
// rendition, and finds where their media playlists are. False if the master is
// not the shape expected.
bool TrailerPlaylistServer::parseMaster(Trailer &trailer, const QByteArray &text,
                                        QUrl &videoUrl, QUrl &audioUrl) const {
    static const QRegularExpression uriAttr(QStringLiteral("URI=\"([^\"]*)\""));
    static const QRegularExpression bandwidthAttr(QStringLiteral("[:,]BANDWIDTH=(\\d+)"));

    const QUrl base(trailer.master);
    const QStringList lines = QString::fromUtf8(text).split('\n');
    if (lines.isEmpty() || !lines.first().trimmed().startsWith("#EXTM3U"))
        return false;

    QStringList head;
    QString bestInf, bestUri, audioMedia, audioUri;
    qlonglong bestBandwidth = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.startsWith("#EXT-X-STREAM-INF:")) {
            // The variant's URI is the next line that is not a tag.
            int next = i + 1;
            while (next < lines.size()
                   && (lines.at(next).trimmed().isEmpty() || lines.at(next).trimmed().startsWith('#')))
                ++next;
            if (next >= lines.size())
                break;
            const qlonglong bandwidth = bandwidthAttr.match(line).captured(1).toLongLong();
            if (bandwidth > bestBandwidth) {
                bestBandwidth = bandwidth;
                bestInf = line;
                bestUri = absolute(base, lines.at(next).trimmed());
            }
            i = next;
        } else if (line.startsWith("#EXT-X-MEDIA:")) {
            const QRegularExpressionMatch uri = uriAttr.match(line);
            if (line.contains("TYPE=AUDIO") && uri.hasMatch() && audioMedia.isEmpty()) {
                audioUri = absolute(base, uri.captured(1));
                audioMedia = QString(line).replace(uri.capturedStart(1), uri.capturedLength(1), "%URI%");
            }
        } else if (line.startsWith('#') && !line.startsWith("#EXT-X-I-FRAME-STREAM-INF")) {
            head << line;
        }
    }
    if (bestUri.isEmpty())
        return false;

    trailer.masterHead = head;
    trailer.variantInf = bestInf;
    trailer.audioMedia = audioMedia;
    videoUrl = QUrl(bestUri);
    audioUrl = audioUri.isEmpty() ? QUrl() : QUrl(audioUri);
    return true;
}

// A media playlist as its tags, init section and chunks, every URI absolute so
// they resolve against Steam rather than 127.0.0.1.
bool TrailerPlaylistServer::parseMedia(Media &media, const QByteArray &text, const QUrl &base) {
    static const QRegularExpression uriAttr(QStringLiteral("URI=\"([^\"]*)\""));
    static const QRegularExpression extinf(QStringLiteral("^#EXTINF:([0-9.]+)"));

    media = Media();
    const QStringList lines = QString::fromUtf8(text).split('\n');
    double duration = -1;
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith("#EXTINF:")) {
            duration = extinf.match(line).captured(1).toDouble();
        } else if (!line.startsWith('#')) {
            if (duration >= 0)
                media.chunks.append({ duration, absolute(base, line) });
            duration = -1;
        } else if (line.startsWith("#EXT-X-MAP:")) {
            QString map = line;
            const QRegularExpressionMatch uri = uriAttr.match(map);
            if (uri.hasMatch())
                map.replace(uri.capturedStart(1), uri.capturedLength(1), absolute(base, uri.captured(1)));
            media.map = map;
        } else if (!line.startsWith("#EXT-X-MEDIA-SEQUENCE") && !line.startsWith("#EXT-X-ENDLIST")) {
            media.head << line;
        }
    }
    return !media.chunks.isEmpty();
}

QByteArray TrailerPlaylistServer::renderMedia(const Media &media, int first) {
    QStringList lines = media.head;
    lines << QStringLiteral("#EXT-X-MEDIA-SEQUENCE:%1").arg(first);
    if (!media.map.isEmpty())
        lines << media.map;
    for (int i = first; i < media.chunks.size(); ++i)
        lines << QStringLiteral("#EXTINF:%1,").arg(media.chunks.at(i).first) << media.chunks.at(i).second;
    lines << "#EXT-X-ENDLIST";
    return (lines.join('\n') + '\n').toUtf8();
}

// The chunk holding `seconds`, or the last one past the end.
int TrailerPlaylistServer::chunkAt(const Media &media, double seconds) {
    double start = 0;
    for (int i = 0; i < media.chunks.size(); ++i) {
        start += media.chunks.at(i).first;
        if (seconds < start - 0.001)
            return i;
    }
    return qMax(0, int(media.chunks.size()) - 1);
}

double TrailerPlaylistServer::startOf(const Media &media, int chunk) {
    double start = 0;
    for (int i = 0; i < chunk && i < media.chunks.size(); ++i)
        start += media.chunks.at(i).first;
    return start;
}

void TrailerPlaylistServer::reply(QTcpSocket *socket, const QByteArray &status,
                                  const QByteArray &headers, const QByteArray &body) {
    socket->write("HTTP/1.1 " + status + "\r\n" + headers
                  + "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                  + "Connection: close\r\n\r\n" + body);
    socket->disconnectFromHost();
}
