#ifndef TRAILER_PLAYLIST_SERVER_H
#define TRAILER_PLAYLIST_SERVER_H

// ─────────────────────────────────────────────────────────────────────────────
// trailer_playlist_server.h — hands Qt's player Steam trailers it can play.
//
// Steam's trailers are HLS: a master playlist listing the video four times
// over (1080p, 720p, 480p, 360p) plus a separate audio rendition, each a media
// playlist of 3-second fMP4 chunks. FFmpeg's HLS demuxer, as Qt drives it, has
// two problems with that shape:
//
//   - It downloads every chunk of every variant -- five requests per three
//     seconds of video, one after another on the thread that feeds the
//     decoder -- and plays only one of them. Any slow request among them
//     stalls the picture, which is why long trailers stutter.
//   - It cannot seek. After a seek it reopens the audio rendition only and
//     never the video, so the picture freezes for good.
//
// So this serves its own playlists from 127.0.0.1: a master with only the
// best variant and the audio, and media playlists that start at any chunk.
// Seeking is then a fresh open of a playlist starting at the chunk wanted
// (see seek()), which FFmpeg handles fine. The chunks themselves still stream
// straight from Steam's CDN. It has to be real HTTP with .m3u8 URLs, because
// FFmpeg will not take an HLS playlist from a data: URL.
//
// Only masters the bridge registered are served, so the endpoint cannot be
// used to fetch anything else. A master that cannot be fetched or understood
// answers with a redirect to Steam's original, so the worst case is the old
// behaviour.
// ─────────────────────────────────────────────────────────────────────────────

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <functional>

class QNetworkAccessManager;
class QTcpServer;
class QTcpSocket;

class TrailerPlaylistServer : public QObject {
    Q_OBJECT
public:
    TrailerPlaylistServer(QNetworkAccessManager *network, QObject *parent = nullptr);

    // The local URL to play `master` from, starting the server on first use.
    // `master` itself when the server cannot listen, so playback still works.
    QString localUrlFor(const QString &master);

    // Where to play `localUrl`'s trailer from to land at `positionMs`:
    // {url, offsetMs, durationMs}, the URL starting at the chunk holding that
    // moment and offsetMs where that chunk starts, which the player adds to
    // its own position. Empty for a URL this did not hand out, or before its
    // playlists have been read.
    QVariantMap seek(const QString &localUrl, qint64 positionMs) const;

private:
    struct Media {
        QStringList head;                           // tags kept as they are
        QString map;                                // EXT-X-MAP, URI made absolute
        QList<QPair<double, QString>> chunks;       // seconds, absolute URI
    };
    struct Trailer {
        QString master;
        bool loaded = false;
        bool loading = false;
        QStringList masterHead;
        QString variantInf;
        QString audioMedia;                         // EXT-X-MEDIA line, URI placeholder
        Media video, audio;
        QList<std::function<void(bool)>> waiting;
    };

    QNetworkAccessManager  *m_network;
    QTcpServer             *m_server = nullptr;
    QHash<QString, int>     m_ids;                  // master URL -> id
    QHash<int, Trailer>     m_trailers;

    bool ensureListening();
    QString urlFor(int id, int chunk, const char *document) const;
    void handleConnection(QTcpSocket *socket);
    void serve(QTcpSocket *socket, int id, int chunk, const QString &document);
    void load(int id, std::function<void(bool)> done);
    void finishLoad(int id, bool ok);
    void fetch(const QUrl &url, std::function<void(bool, QByteArray)> done);
    bool parseMaster(Trailer &trailer, const QByteArray &text, QUrl &videoUrl, QUrl &audioUrl) const;
    static bool parseMedia(Media &media, const QByteArray &text, const QUrl &base);
    static QByteArray renderMedia(const Media &media, int first);
    static int chunkAt(const Media &media, double seconds);
    static double startOf(const Media &media, int chunk);
    static void reply(QTcpSocket *socket, const QByteArray &status,
                      const QByteArray &headers, const QByteArray &body = {});
};

#endif // TRAILER_PLAYLIST_SERVER_H
