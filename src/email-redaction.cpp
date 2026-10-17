/** @fileoverview Validates OCR word geometry and complete email tokens. */
#include "email-redaction.hpp"

#include <QChar>
#include <QRect>
#include <QRegularExpression>
#include <QSize>
#include <QString>
#include <QStringView>
#include <QVector>
#include <QtGlobal>

#include <array>
#include <cmath>
#include <cstddef>

namespace {
bool isEmailToken(QStringView token) {
  token = token.trimmed();
  bool removedPunctuation = false;
  while (token.size() >= 2) {
    const QChar first = token.front();
    const QChar last = token.back();
    const bool wrapped =
        (first == '(' && last == ')') || (first == '[' && last == ']') ||
        (first == '{' && last == '}') || (first == '<' && last == '>') ||
        (first == '"' && last == '"') || (first == '\'' && last == '\'');
    if (wrapped) {
      token = token.mid(1, token.size() - 2);
      continue;
    }
    if (!removedPunctuation && QStringView(u",.;:!?").contains(last)) {
      token.chop(1);
      removedPunctuation = true;
      continue;
    }
    break;
  }

  const qsizetype at = token.indexOf('@');
  if (at < 1 || at > 64 || token.size() > 254 ||
      token.size() - at - 1 > 253)
    return false;

  // Match the whole word: a bad prefix (such as a doubled local-part dot)
  // must not turn into a valid-looking suffix and receive a partial redaction.
  static const QRegularExpression email(
      QStringLiteral(
          R"(\A[A-Za-z0-9!#$%&'*+/=?^_`{|}~-]+(?:\.[A-Za-z0-9!#$%&'*+/=?^_`{|}~-]+)*@(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63}\z)"));
  return email.matchView(token).hasMatch();
}
} // namespace

QVector<QRect> emailAddressRegions(const QString &tsv, const QSize &imageSize) {
  QVector<QRect> regions;
  if (imageSize.isEmpty())
    return regions;

  const auto lines = QStringView(tsv).split('\n');
  for (QStringView line : lines) {
    if (line.endsWith('\r'))
      line.chop(1);
    const auto fields = line.split('\t');
    if (fields.size() != 12)
      continue;

    // A TSV word is level 5. Validate all hierarchy and geometry fields so
    // malformed output cannot paint an unrelated part of the screenshot.
    std::array<int, 10> numbers{};
    bool valid = true;
    for (qsizetype index = 0; index < 10; ++index) {
      bool ok = false;
      numbers.at(static_cast<std::size_t>(index)) = fields[index].toInt(&ok);
      valid = valid && ok;
    }
    if (!valid || numbers[0] != 5)
      continue;
    for (std::size_t index = 1; index < 6; ++index)
      valid = valid && numbers[index] > 0;
    bool confidenceOk = false;
    const double confidence = fields[10].toDouble(&confidenceOk);
    if (!valid || !confidenceOk || !std::isfinite(confidence) ||
        confidence < 0.0 || confidence > 100.0)
      continue;

    const int left = numbers[6];
    const int top = numbers[7];
    const int width = numbers[8];
    const int height = numbers[9];
    if (left < 0 || top < 0 || width <= 0 || height <= 0 ||
        left >= imageSize.width() || top >= imageSize.height() ||
        width > imageSize.width() - left ||
        height > imageSize.height() - top || !isEmailToken(fields[11]))
      continue;

    const QRect region(left, top, width, height);
    if (!regions.contains(region))
      regions.push_back(region);
  }
  return regions;
}
