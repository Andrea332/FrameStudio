#pragma once

#include <QString>

#include <optional>

struct MediaInfo {
    QString path;
    double duration = 0;
    double startTime = 0;       // timestamp where the file starts (e.g. 1.4 s in MPEG-TS), times are relative to it
    int width = 0;              // as displayed (rotation already applied)
    int height = 0;
    int rotation = 0;           // degrees clockwise to show the stored frames upright (phone videos)
    int videoIndex = 0;         // stream index of the main video (cover art is skipped)
    double fps = 0;
    bool hasAudio = false;
    int audioChannels = 0;      // of the first audio track
    int audioKbps = 0;          // of the first audio track, 0 if unknown
    int videoKbps = 0;          // of the video track (or of the whole file), 0 if unknown
    qint64 size = 0;
};

// Prefers the FFmpeg bundled with the app (ffmpeg/ folder next to the executable), otherwise uses PATH.
QString findTool(const QString &name);

// Reads duration, dimensions, fps and audio presence with ffprobe. On failure returns nothing and sets *error.
std::optional<MediaInfo> probe(const QString &ffprobe, const QString &path, QString *error);

double parseRate(const QString &text);
QString fmtTime(double seconds);
QString fmtMb(double bytes);
