#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

// The lyrics are cut into sections. A section is a block of text lines [first, last) that is sung during
// [t0, t1) of the song (time in beats from the start of the song, so tempo changes keep the marks on their bars).
// Rules: sections never overlap in time; two sections that touch ("a joint") move together when the joint is dragged.
// Plain C++ (no JUCE), so the rules can be tested on their own.
struct LyricsSection
{
    int first = 0, last = 0;     // text lines [first, last)
    double t0 = 0.0, t1 = 0.0;   // beats
};

class LyricsSections
{
public:
    static constexpr double minLength = 0.25;      // beats: the shortest section
    static constexpr double jointEpsilon = 1.0e-6;

    std::vector<LyricsSection> items;

    int size() const { return (int) items.size(); }
    bool empty() const { return items.empty(); }
    void clear() { items.clear(); }

    // First text line that no section has used yet (where the next block starts).
    int linesEnd() const
    {
        int e = 0;
        for (auto& s : items)
            e = std::max (e, s.last);
        return e;
    }

    // The section that is sung at this time, -1 in a gap.
    int sectionAt (double t) const
    {
        for (int i = 0; i < size(); ++i)
            if (t >= items[(size_t) i].t0 && t < items[(size_t) i].t1)
                return i;
        return -1;
    }

    // Number of sections that are completely over at this time.
    int numFinished (double t) const
    {
        int n = 0;
        for (auto& s : items)
            if (s.t1 <= t)
                ++n;
        return n;
    }

    int sectionOfLine (int line) const
    {
        for (int i = 0; i < size(); ++i)
            if (line >= items[(size_t) i].first && line < items[(size_t) i].last)
                return i;
        return -1;
    }

    // New section [first, last) that starts at time t. Returns its index, -1 if it does not fit
    // (a marker already sits there). The section before it ends where the new one starts; the new one ends
    // where the next one starts or at defaultEnd.
    int add (int first, int last, double t, double defaultEnd)
    {
        if (last <= first || t < 0.0)
            return -1;

        int at = 0;
        while (at < size() && items[(size_t) at].t0 <= t)
            ++at;

        if (at > 0 && t - items[(size_t) at - 1].t0 < minLength)
            return -1;                       // too close to the start of the previous section

        double end = std::max (defaultEnd, t + minLength);
        if (at < size())
        {
            end = std::min (end, items[(size_t) at].t0);
            if (end - t < minLength)
                return -1;                   // no room before the next section
        }

        if (at > 0 && items[(size_t) at - 1].t1 > t)
            items[(size_t) at - 1].t1 = t;   // the previous section ends where the new one starts

        LyricsSection s;
        s.first = first;
        s.last = last;
        s.t0 = t;
        s.t1 = end;
        items.insert (items.begin() + at, s);
        return at;
    }

    void remove (int i)
    {
        if (i >= 0 && i < size())
            items.erase (items.begin() + i);
    }

    // ---- time

    bool touchesPrevious (int i) const
    {
        return i > 0 && std::abs (items[(size_t) i - 1].t1 - items[(size_t) i].t0) <= jointEpsilon;
    }

    bool touchesNext (int i) const
    {
        return i + 1 < size() && std::abs (items[(size_t) i].t1 - items[(size_t) i + 1].t0) <= jointEpsilon;
    }

    // start of section i (a joint with the previous section moves both)
    void moveStart (int i, double t)
    {
        if (i < 0 || i >= size())
            return;
        auto& s = items[(size_t) i];
        const bool joint = touchesPrevious (i);
        double lo = 0.0;
        if (i > 0)
            lo = joint ? items[(size_t) i - 1].t0 + minLength : items[(size_t) i - 1].t1;
        const double hi = s.t1 - minLength;
        t = std::max (lo, std::min (hi, t));
        if (joint)
            items[(size_t) i - 1].t1 = t;
        s.t0 = t;
    }

    // end of section i (a joint with the next section moves both)
    void moveEnd (int i, double t, double maxTime)
    {
        if (i < 0 || i >= size())
            return;
        auto& s = items[(size_t) i];
        const bool joint = touchesNext (i);
        double hi = maxTime;
        if (i + 1 < size())
            hi = joint ? items[(size_t) i + 1].t1 - minLength : items[(size_t) i + 1].t0;
        const double lo = s.t0 + minLength;
        t = std::max (lo, std::min (std::max (lo, hi), t));
        if (joint)
            items[(size_t) i + 1].t0 = t;
        s.t1 = t;
    }

    // the whole section in time, as far as the neighbours let it (it never pushes them)
    void moveBody (int i, double dt, double maxTime)
    {
        if (i < 0 || i >= size())
            return;
        auto& s = items[(size_t) i];
        const double lo = i > 0 ? items[(size_t) i - 1].t1 : 0.0;
        const double hi = i + 1 < size() ? items[(size_t) i + 1].t0 : maxTime;
        const double len = s.t1 - s.t0;
        double start = s.t0 + dt;
        start = std::max (lo, std::min (std::max (lo, hi - len), start));
        s.t0 = start;
        s.t1 = start + len;
    }

    // ---- text lines

    bool textTouchesPrevious (int i) const
    {
        return i > 0 && items[(size_t) i - 1].last == items[(size_t) i].first;
    }

    bool textTouchesNext (int i) const
    {
        return i + 1 < size() && items[(size_t) i].last == items[(size_t) i + 1].first;
    }

    // first line of section i (a text joint with the previous section moves both)
    void moveFirst (int i, int line)
    {
        if (i < 0 || i >= size())
            return;
        auto& s = items[(size_t) i];
        const bool joint = textTouchesPrevious (i);
        const int lo = joint ? items[(size_t) i - 1].first + 1 : 0;
        const int hi = s.last - 1;
        line = std::max (lo, std::min (std::max (lo, hi), line));
        if (joint)
            items[(size_t) i - 1].last = line;
        s.first = line;
    }

    // end of section i (exclusive)
    void moveLast (int i, int line, int totalLines)
    {
        if (i < 0 || i >= size())
            return;
        auto& s = items[(size_t) i];
        const bool joint = textTouchesNext (i);
        const int hi = joint ? items[(size_t) i + 1].last - 1 : totalLines;
        const int lo = s.first + 1;
        line = std::max (lo, std::min (std::max (lo, hi), line));
        if (joint)
            items[(size_t) i + 1].first = line;
        s.last = line;
    }

    // the whole block of text moves up / down; neighbours that touch it give / take lines
    void shiftLines (int i, int delta, int totalLines)
    {
        if (i < 0 || i >= size() || delta == 0)
            return;
        auto& s = items[(size_t) i];
        const bool jp = textTouchesPrevious (i), jn = textTouchesNext (i);

        int lo = -s.first;                                   // block must stay inside the text
        int hi = totalLines - s.last;
        if (jp)
            lo = std::max (lo, items[(size_t) i - 1].first + 1 - s.first);   // the neighbour keeps one line
        if (jn)
            hi = std::min (hi, items[(size_t) i + 1].last - 1 - s.last);
        delta = std::max (lo, std::min (std::max (lo, hi), delta));
        if (delta == 0)
            return;

        s.first += delta;
        s.last += delta;
        if (jp)
            items[(size_t) i - 1].last = s.first;
        if (jn)
            items[(size_t) i + 1].first = s.last;
    }

    // The text changed: `delta` lines were added (+) or removed (-) after the first `keptLines` lines.
    void textEdited (int keptLines, int delta, int totalLines)
    {
        if (delta == 0)
            return;
        auto fix = [&] (int& x)
        {
            if (x > keptLines)
                x = delta > 0 ? x + delta : std::max (keptLines, x + delta);
            x = std::max (0, std::min (totalLines, x));
        };
        for (auto& s : items)
        {
            fix (s.first);
            fix (s.last);
        }
        items.erase (std::remove_if (items.begin(), items.end(), [] (const LyricsSection& s) { return s.last <= s.first; }), items.end());
    }
};
