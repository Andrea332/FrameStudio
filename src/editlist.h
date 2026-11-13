#pragma once

#include <utility>
#include <vector>

// A piece of the source video, from start to end in seconds of the source.
struct Piece {
    double start = 0;
    double end = 0;

    double length() const { return end - start; }
    bool operator==(const Piece &) const = default;
};

// The edited video: pieces of the source, played one after the other. Positions on the timeline are seconds
// of the edited video. There are never gaps: removing a part closes the gap, inserting pushes what follows.
class EditList
{
public:
    EditList() = default;
    explicit EditList(double sourceDuration);   // the whole source, in one piece

    const std::vector<Piece> &pieces() const { return pieces_; }
    void setPieces(std::vector<Piece> pieces);
    double duration() const;
    bool isWhole(double sourceDuration) const;   // nothing edited: the whole source, in order
    // the same video with neighbouring pieces that continue each other joined (what gets encoded and played)
    std::vector<Piece> joined() const;

    // the piece at timeline position t (t is clamped to the video) and the source time there
    std::pair<int, double> locate(double t) const;
    double pieceStart(int index) const;          // timeline position where a piece starts

    void split(double t);                        // two pieces from the one at t (nothing on a boundary)
    std::vector<Piece> copy(double from, double to) const;   // the parts of the pieces in [from, to)
    std::vector<Piece> remove(double from, double to);       // removes [from, to) and returns it
    void insert(double at, const std::vector<Piece> &pieces);
    // moves [from, to) to where `at` is now (nothing if `at` is inside it); returns its new start
    double move(double from, double to, double at);

private:
    int boundary(double t);   // splits the piece at t if needed; returns the index of the piece starting at t

    std::vector<Piece> pieces_;
};
