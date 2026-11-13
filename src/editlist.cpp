#include "editlist.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double EPSILON = 1e-6;   // seconds: positions this close are the same

} // namespace

EditList::EditList(double sourceDuration)
{
    if (sourceDuration > EPSILON)
        pieces_.push_back({0, sourceDuration});
}

void EditList::setPieces(std::vector<Piece> pieces)
{
    std::erase_if(pieces, [](const Piece &p) { return p.length() < EPSILON; });
    pieces_ = std::move(pieces);
}

double EditList::duration() const
{
    double total = 0;
    for (const Piece &p : pieces_)
        total += p.length();
    return total;
}

bool EditList::isWhole(double sourceDuration) const
{
    const std::vector<Piece> all = joined();
    return all.size() == 1 && all[0].start < EPSILON && all[0].end > sourceDuration - EPSILON;
}

std::vector<Piece> EditList::joined() const
{
    std::vector<Piece> result;
    for (const Piece &p : pieces_) {
        if (!result.empty() && std::abs(result.back().end - p.start) < EPSILON)
            result.back().end = p.end;
        else
            result.push_back(p);
    }
    return result;
}

std::pair<int, double> EditList::locate(double t) const
{
    if (pieces_.empty())
        return {-1, 0};
    double position = 0;
    for (size_t i = 0; i < pieces_.size(); ++i) {
        const double length = pieces_[i].length();
        if (t < position + length - EPSILON || i + 1 == pieces_.size())
            return {int(i), pieces_[i].start + std::clamp(t - position, 0.0, length)};
        position += length;
    }
    return {-1, 0};
}

double EditList::pieceStart(int index) const
{
    double position = 0;
    for (int i = 0; i < index && i < int(pieces_.size()); ++i)
        position += pieces_[size_t(i)].length();
    return position;
}

int EditList::boundary(double t)
{
    double position = 0;
    for (size_t i = 0; i < pieces_.size(); ++i) {
        if (t <= position + EPSILON)
            return int(i);
        const double length = pieces_[i].length();
        if (t < position + length - EPSILON) {
            const double at = pieces_[i].start + (t - position);
            const Piece second{at, pieces_[i].end};
            pieces_[i].end = at;
            pieces_.insert(pieces_.begin() + std::ptrdiff_t(i) + 1, second);
            return int(i) + 1;
        }
        position += length;
    }
    return int(pieces_.size());
}

void EditList::split(double t)
{
    boundary(t);
}

std::vector<Piece> EditList::copy(double from, double to) const
{
    EditList copy = *this;
    return copy.remove(from, to);
}

std::vector<Piece> EditList::remove(double from, double to)
{
    if (to < from)
        std::swap(from, to);
    const int first = boundary(from);
    const int last = boundary(to);
    std::vector<Piece> removed(pieces_.begin() + first, pieces_.begin() + last);
    pieces_.erase(pieces_.begin() + first, pieces_.begin() + last);
    return removed;
}

void EditList::insert(double at, const std::vector<Piece> &pieces)
{
    const int index = boundary(at);
    pieces_.insert(pieces_.begin() + index, pieces.begin(), pieces.end());
}

double EditList::move(double from, double to, double at)
{
    if (to < from)
        std::swap(from, to);
    if (at >= from - EPSILON && at <= to + EPSILON)
        return from;
    const std::vector<Piece> moved = remove(from, to);
    const double start = at > to ? at - (to - from) : at;
    insert(start, moved);
    return start;
}
