// Qt's JavaScript heap lives in memfd mappings: 2000 strings held only by a JS array must not show
// as leaked in a snapshot taken while they are held. Prints "held", quits after 3 s.
import QtQml
QtObject {
    property var keep: []
    property Timer t: Timer { interval: 3000; running: true; onTriggered: Qt.quit() }
    Component.onCompleted: {
        for (let i = 0; i < 2000; i++) keep.push("held by the JS heap, entry " + i + " ".repeat(200))
        console.log("held")
    }
}
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AMARBARO · amarbaro.org labs
