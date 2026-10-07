import QtQuick
import QtWebEngine
import Vortex

// One IGDB trailer, played through YouTube's own embedded player -- the
// fallback for a game Steam has no trailers for.
//
// Chromium is heavy (a renderer and a GPU process, ~150-300 MB while a video
// plays), so this exists only while the page's trailer lightbox shows a
// YouTube video: BrowseDetails' Loader creates it on that click and destroys
// it on close, and the renderer process exits with it. Nothing autoplays it.
Item {
    id: youtube

    property string videoId
    property bool startMuted: false

    // YouTube's error code (2, 5, 100, 101, 150, 153 -- 101 and 150 are "the
    // owner does not allow embedding"), or -1 for a dead renderer. 0 is fine.
    property int errorCode: 0

    readonly property string watchUrl: "https://www.youtube.com/watch?v=" + youtube.videoId

    function togglePlay() {
        view.runJavaScript("window.vortexToggle && window.vortexToggle()")
    }

    // An https origin to load the page under, so the embed is sent with a
    // Referer: YouTube refuses a player that has none (error 153). Nothing is
    // ever fetched from it.
    readonly property url pageOrigin: "https://vortex.localhost/"

    // YouTube's IFrame API rather than a bare iframe, for its error and state
    // events, which come back through the document title.
    function pageFor(id) {
        return "<!doctype html><html><head><meta charset='utf-8'>"
            + "<style>html,body{margin:0;height:100%;background:#000;overflow:hidden}"
            + "#player{width:100%;height:100%}</style></head><body><div id='player'></div><script>"
            + "var p;"
            + "window.vortexToggle=function(){if(!p||!p.getPlayerState)return;"
            + "p.getPlayerState()===1?p.pauseVideo():p.playVideo()};"
            + "function onYouTubeIframeAPIReady(){p=new YT.Player('player',{"
            + "host:'https://www.youtube-nocookie.com',videoId:" + JSON.stringify(id) + ","
            + "playerVars:{autoplay:1,rel:0,fs:0,playsinline:1,iv_load_policy:3,origin:location.origin},"
            + "events:{onReady:function(e){" + (youtube.startMuted ? "e.target.mute();" : "")
            + "e.target.playVideo()},"
            + "onError:function(e){document.title='error:'+e.data}}})}"
            + "var s=document.createElement('script');s.src='https://www.youtube.com/iframe_api';"
            + "document.head.appendChild(s);"
            + "</script></body></html>"
    }

    function load() {
        youtube.errorCode = 0
        if (youtube.videoId !== "")
            view.loadHtml(youtube.pageFor(youtube.videoId), youtube.pageOrigin)
    }

    onVideoIdChanged: youtube.load()
    Component.onCompleted: youtube.load()

    WebEngineView {
        id: view
        anchors.fill: parent
        backgroundColor: "black"

        // The default profile, which in Qt 6 is off the record: no cookies,
        // history or cache written to disk.
        settings.playbackRequiresUserGesture: false
        settings.showScrollBars: false

        onTitleChanged: {
            const match = /^error:(\d+)$/.exec(view.title)
            if (match) youtube.errorCode = parseInt(match[1])
        }

        // The YouTube logo, the title and "watch on YouTube" all try to leave
        // the player; they go to the browser instead.
        onNewWindowRequested: (request) => Qt.openUrlExternally(request.requestedUrl)
        onNavigationRequested: (request) => {
            if (request.isMainFrame
                    && request.navigationType === WebEngineNavigationRequest.LinkClickedNavigation) {
                request.reject()
                Qt.openUrlExternally(request.url)
            }
        }

        onRenderProcessTerminated: youtube.errorCode = -1
    }

    // YouTube's own error screen is easy to miss and offers nothing to do;
    // this one offers the browser.
    Rectangle {
        anchors.fill: parent
        visible: youtube.errorCode !== 0
        color: "black"

        Column {
            anchors.centerIn: parent
            spacing: 18

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: youtube.errorCode === 101 || youtube.errorCode === 150
                      ? "This video can only be watched on YouTube."
                      : "This video couldn't be played here."
                color: Theme.textBody
                font.pixelSize: 15
            }

            Rectangle {
                id: openButton
                anchors.horizontalCenter: parent.horizontalCenter
                width: openLabel.implicitWidth + 36; height: 44; radius: 8
                color: openArea.containsMouse ? Theme.bgEmphasis : Theme.bgActive
                border.width: openArea.containsMouse ? 2 : 0
                border.color: Theme.focusRing
                Behavior on color { ColorAnimation { duration: 150 } }

                Text {
                    id: openLabel
                    anchors.centerIn: parent
                    text: "OPEN ON YOUTUBE"
                    color: Theme.textPrimary
                    font.pixelSize: 12; font.bold: true; font.letterSpacing: 1
                }
                MouseArea {
                    id: openArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Qt.openUrlExternally(youtube.watchUrl)
                }
            }
        }
    }
}
