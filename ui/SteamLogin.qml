pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtWebEngine
import Vortex

// The one-time Steam sign-in behind the Library's NOT INSTALLED toggle.
//
// Steam keeps no list of what an account owns on disk, and the Web API that
// has one wants either a developer key or the access token Steam's own store
// hands a signed-in session. This is that session: the player signs in on
// Steam's real login page, Steam Guard and all, and Vortex never sees the
// password. The session's cookies stay in a WebEngine profile of their own --
// not YouTubePlayer's off-the-record one -- and only the short-lived Web API
// token read from it is handed to the bridge, for one request at a time.
//
// Chromium is heavy, so neither view exists until it is needed: the login
// page while its popup is up, the token fetch for the second it takes.
Item {
    id: steam

    property var api: vortexApi

    // The persistent profile the session lives in. Passed to the bridge on
    // sign-out, which clears its cookies from C++ (the QML profile has no
    // cookie API of its own). Taken once the prototype is complete, so the
    // profile is built with the storage settings below and not the defaults.
    property var profile: null
    Component.onCompleted: steam.profile = steamProfile.instance()

    function signIn() {
        loginPopup.open()
    }

    function signOut() {
        if (steam.api)
            steam.api.signOutSteam(steam.profile)
    }

    // Reads a fresh token from the session and passes it on.
    function refresh() {
        if (!tokenFetch.active)
            tokenFetch.active = true
    }

    WebEngineProfilePrototype {
        id: steamProfile
        storageName: "VortexSteam"
        // Steam's session cookie can be a session-only one; without this it
        // would be dropped at exit and the sign-in would not be one-time.
        persistentCookiesPolicy: WebEngineProfile.ForcePersistentCookies
        httpCacheType: WebEngineProfile.MemoryHttpCache
    }

    Connections {
        target: steam.api
        function onSteamTokenWanted() { steam.refresh() }
    }

    // ── Token fetch ─────────────────────────────────────────────────────────
    // The store's own async config carries the session's Web API token (the
    // one store pages use to call the Web API themselves). Empty when the
    // session is gone.
    readonly property url tokenUrl: "https://store.steampowered.com/pointssummary/ajaxgetasyncconfig"

    function finishFetch(text) {
        // Not torn down from inside the view's own signal.
        Qt.callLater(function() { tokenFetch.active = false })
        if (!steam.api || text === null)
            return      // offline or Steam down: keep the cached list as it is
        let token = ""
        try {
            const reply = JSON.parse(text)
            token = (reply && reply.data && reply.data.webapi_token) || ""
        } catch (e) {
            // Not JSON: Steam answered with a page, which means no session.
        }
        steam.api.setSteamWebToken(token)
    }

    Loader {
        id: tokenFetch
        active: false
        sourceComponent: WebEngineView {
            id: fetchView
            visible: false
            width: 1; height: 1
            profile: steam.profile
            url: steam.tokenUrl

            onLoadingChanged: (info) => {
                if (info.status === WebEngineLoadingInfo.LoadSucceededStatus)
                    fetchView.runJavaScript("document.body ? document.body.innerText : ''",
                                            function(text) { steam.finishFetch(text) })
                else if (info.status === WebEngineLoadingInfo.LoadFailedStatus)
                    steam.finishFetch(null)
            }
            onRenderProcessTerminated: steam.finishFetch(null)
        }
    }

    // ── Login page ──────────────────────────────────────────────────────────
    // Only Steam's own sites load in here; a link anywhere else goes to the
    // browser, so the window can never turn into a general-purpose one.
    function hostOf(address) {
        const match = /^https?:\/\/([^\/?#:]+)/.exec(String(address))
        return match ? match[1].toLowerCase() : ""
    }
    function pathOf(address) {
        const match = /^https?:\/\/[^\/?#]+(\/[^?#]*)?/.exec(String(address))
        return match && match[1] ? match[1] : "/"
    }
    function isSteamHost(host) {
        return host === "steampowered.com" || host.endsWith(".steampowered.com")
            || host === "steamcommunity.com" || host.endsWith(".steamcommunity.com")
            || host.endsWith(".steamstatic.com")
    }

    Popup {
        id: loginPopup
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(560, (parent ? parent.width : 560) - 80)
        height: Math.min(780, (parent ? parent.height : 780) - 80)
        // Above Settings, which is where this is usually opened from.
        z: 100
        modal: true
        dim: true
        padding: 0
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: Theme.bgSurface
            radius: 12
            border.color: Theme.borderQuiet
            border.width: 1
        }

        contentItem: Item {
            // Where the page is, always visible, so the player can see they
            // are typing into Steam and nowhere else.
            Rectangle {
                id: addressBar
                anchors { left: parent.left; right: parent.right; top: parent.top }
                height: 48
                radius: 12
                color: Theme.steamBg

                // Squares off the bottom corners against the page.
                Rectangle {
                    anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                    height: parent.radius
                    color: parent.color
                }

                Image {
                    id: steamMark
                    anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                    source: "assets/steam.png"
                    sourceSize.width: 20; sourceSize.height: 20
                    width: 20; height: 20
                    smooth: true
                }

                Text {
                    anchors {
                        left: steamMark.right; leftMargin: 10
                        right: closeButton.left; rightMargin: 10
                        verticalCenter: parent.verticalCenter
                    }
                    text: loginLoader.item
                          ? ((String(loginLoader.item.url).startsWith("https://") ? "\u{1F512} " : "")
                             + steam.hostOf(loginLoader.item.url))
                          : ""
                    color: Theme.steamAccent
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }

                Text {
                    id: closeButton
                    anchors { right: parent.right; rightMargin: 16; verticalCenter: parent.verticalCenter }
                    text: "✕"
                    color: closeArea.containsMouse ? Theme.textPrimary : Theme.textMuted
                    font.pixelSize: 16
                    MouseArea {
                        id: closeArea
                        anchors.fill: parent
                        anchors.margins: -8
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: loginPopup.close()
                    }
                }
            }

            Loader {
                id: loginLoader
                anchors {
                    left: parent.left; right: parent.right
                    top: addressBar.bottom; bottom: parent.bottom
                    margins: 1
                }
                active: loginPopup.visible
                sourceComponent: WebEngineView {
                    id: loginView
                    profile: steam.profile
                    backgroundColor: Theme.steamBg
                    url: "https://store.steampowered.com/login/"

                    // Steam leaves the login page once the session is up.
                    onUrlChanged: {
                        if (steam.hostOf(loginView.url) === "store.steampowered.com"
                                && !steam.pathOf(loginView.url).startsWith("/login")) {
                            loginPopup.close()
                            steam.refresh()
                        }
                    }

                    onNavigationRequested: (request) => {
                        if (request.isMainFrame && !steam.isSteamHost(steam.hostOf(request.url))) {
                            request.reject()
                            Qt.openUrlExternally(request.url)
                        }
                    }
                    onNewWindowRequested: (request) => Qt.openUrlExternally(request.requestedUrl)
                }
            }

            Text {
                anchors.centerIn: loginLoader
                visible: !!loginLoader.item && loginLoader.item.loading
                         && loginLoader.item.loadProgress < 60
                text: "Loading Steam…"
                color: Theme.textMuted
                font.pixelSize: 14
            }
        }
    }
}
