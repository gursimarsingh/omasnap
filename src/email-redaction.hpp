/** @fileoverview Finds complete email tokens in in-memory OCR word boxes. */
#pragma once

#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

/** Returns native-pixel boxes only; recognized text never leaves this helper.
 *  Accepts complete conventional ASCII addresses within one TSV word, with
 *  optional surrounding punctuation. Split OCR words are deliberately not
 *  joined, to avoid combining unrelated nearby screenshot text. */
[[nodiscard]] QVector<QRect> emailAddressRegions(const QString &tsv,
                                                const QSize &imageSize);
