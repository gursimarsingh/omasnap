/** @fileoverview Email OCR produces ordinary, reversible solid redactions. */
#include "email-redaction-smoke.hpp"

#include "capture.hpp"
#include "cut.hpp"
#include "editor.hpp"
#include "email-redaction.hpp"

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QKeyEvent>
#include <QList>
#include <QPainter>
#include <QRect>
#include <QRectF>
#include <QScopeGuard>
#include <QSize>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <Qt>
#include <QtGlobal>
#include <QtTest/qtestkeyboard.h>
#include <atomic>
#include <cmath>
#include <memory>

namespace {
template <typename Predicate> bool waitUntil(Predicate ready, int timeout = 5000) {
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < timeout) {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QThread::msleep(1);
  }
  return ready();
}

bool writeFile(const QString &path, const QByteArray &contents, QString &error,
               bool executable = false) {
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      file.write(contents) != contents.size()) {
    error = QStringLiteral("Could not write email smoke fixture: %1").arg(path);
    return false;
  }
  file.close();
  if (executable && !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                                        QFile::ExeOwner)) {
    error = QStringLiteral("Could not make email OCR fixture executable");
    return false;
  }
  return true;
}

QByteArray readFile(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString word(const QString &text, const QRect &rect, int number = 1) {
  return QStringLiteral("5\t1\t1\t1\t1\t%1\t%2\t%3\t%4\t%5\t96\t%6\n")
      .arg(number).arg(rect.x()).arg(rect.y()).arg(rect.width())
      .arg(rect.height()).arg(text);
}

CaptureData fixtureCapture(const QSize &native = QSize(320, 180),
                           const QSize &logical = QSize(320, 180)) {
  CaptureData capture;
  capture.monitor.name = QStringLiteral("TEST");
  capture.monitor.geometry = QRect(QPoint(), logical);
  capture.monitor.pixelSize = native;
  capture.previewSize = logical;
  capture.source = QImage(native, QImage::Format_ARGB32_Premultiplied);
  for (int y = 0; y < native.height(); ++y)
    for (int x = 0; x < native.width(); ++x)
      capture.source.setPixelColor(x, y,
          QColor(80 + x % 160, 70 + y % 160, 90 + (x + y) % 150));
  return capture;
}

Annotation solid(const QRectF &rect) {
  Annotation annotation;
  annotation.kind = Annotation::Kind::Redaction;
  annotation.redactionStyle = RedactionStyle::Solid;
  annotation.start = rect.topLeft();
  annotation.end = rect.bottomRight();
  return annotation;
}

bool nearRect(const QRectF &actual, const QRectF &expected) {
  constexpr qreal tolerance = 0.00001;
  return std::abs(actual.x() - expected.x()) < tolerance &&
         std::abs(actual.y() - expected.y()) < tolerance &&
         std::abs(actual.width() - expected.width()) < tolerance &&
         std::abs(actual.height() - expected.height()) < tolerance;
}

bool parserSmoke(QString &error) {
  const QRect rect(20, 30, 110, 18);
  const QStringList accepted{
      QStringLiteral("alice@example.com"),
      QStringLiteral("ALICE+news@dept.example.co.uk"),
      QStringLiteral("first.last@a-b.example.org"),
      QStringLiteral("(alice@example.com),"),
      QStringLiteral("<alice@example.com>"),
      QStringLiteral("\"alice@example.com\"."),
      QStringLiteral("[alice@example.com]")};
  for (const QString &address : accepted) {
    if (emailAddressRegions(word(address, rect), QSize(300, 200)) !=
        QVector<QRect>{rect}) {
      error = QStringLiteral("Email parser rejected complete token: %1").arg(address);
      return false;
    }
  }
  const QStringList rejected{
      QStringLiteral("plain text"), QStringLiteral("alice@localhost"),
      QStringLiteral("alice@@example.com"), QStringLiteral(".alice@example.com"),
      QStringLiteral("alice..bob@example.com"), QStringLiteral("alice@example..com"),
      QStringLiteral("alice@-example.com"), QStringLiteral("alice@example-.com"),
      QStringLiteral("alice@example.c"), QStringLiteral("alice@example.123"),
      QStringLiteral("mailto:alice@example.com"), QStringLiteral("alice@example.com/path"),
      QStringLiteral("alice@example.com,bob@example.com"),
      QStringLiteral("alice@"), QStringLiteral("example.com"),
      QString::fromUtf8("álîce@example.com")};
  for (const QString &token : rejected) {
    if (!emailAddressRegions(word(token, rect), QSize(300, 200)).isEmpty()) {
      error = QStringLiteral("Email parser accepted malformed or split token: %1").arg(token);
      return false;
    }
  }
  QString malformed = QStringLiteral("level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n");
  for (const QRect &bad : {QRect(-1, 1, 10, 10), QRect(1, -1, 10, 10),
                           QRect(290, 1, 11, 10), QRect(1, 190, 10, 11),
                           QRect(1, 1, 0, 10), QRect(1, 1, 10, 0),
                           QRect(300, 1, 10, 10)})
    malformed += word(QStringLiteral("alice@example.com"), bad);
  const QString valid = word(QStringLiteral("alice@example.com"), rect);
  for (const QString &confidence : {QStringLiteral("nan"), QStringLiteral("inf"),
                                     QStringLiteral("-1"), QStringLiteral("101")}) {
    QString bad = valid;
    bad.replace(QStringLiteral("\t96\t"), '\t' + confidence + '\t');
    malformed += bad;
  }
  QString nonWord = valid;
  nonWord[0] = '4';
  malformed += nonWord;
  QString zeroHierarchy = valid;
  zeroHierarchy.replace(2, 1, QStringLiteral("0"));
  malformed += zeroHierarchy;
  malformed += QStringLiteral("5\t1\t1\t1\t1\t1\t2147483648\t1\t10\t10\t96\talice@example.com\n");
  malformed += valid.trimmed() + QStringLiteral("\textra\n");
  malformed += QStringLiteral("5\t1\t1\talice@example.com\n");
  if (!emailAddressRegions(malformed, QSize(300, 200)).isEmpty() ||
      !emailAddressRegions(valid, QSize()).isEmpty() ||
      emailAddressRegions(valid + valid, QSize(300, 200)) != QVector<QRect>{rect}) {
    error = QStringLiteral("Email parser accepted invalid TSV geometry or duplicate boxes");
    return false;
  }
  QString windowsLine = valid;
  windowsLine.replace('\n', QStringLiteral("\r\n"));
  if (emailAddressRegions(windowsLine, QSize(300, 200)) != QVector<QRect>{rect}) {
    error = QStringLiteral("Email parser lost CRLF input");
    return false;
  }
  return true;
}
} // namespace

bool runEmailRedactionSmoke(QString &error) {
  if (!parserSmoke(error))
    return false;
  const QTemporaryDir directory;
  if (!directory.isValid()) {
    error = QStringLiteral("Could not create email redaction test directory");
    return false;
  }
  const QString realTesseract = QStandardPaths::findExecutable(QStringLiteral("tesseract"));
  const QList<QByteArray> variables{"PATH", "OMASNAP_EMAIL_TEST_DIR", "OMASNAP_EMAIL_TEST_MODE",
                                   "OMASNAP_OCR_LANGS", "OMARCHY_OCR_LANGS"};
  QList<QByteArray> previous;
  for (const QByteArray &name : variables)
    previous.push_back(qgetenv(name.constData()));
  const auto restoreEnvironment = qScopeGuard([&] {
    for (qsizetype i = 0; i < variables.size(); ++i) {
      if (previous.at(i).isNull())
        qunsetenv(variables.at(i).constData());
      else
        qputenv(variables.at(i).constData(), previous.at(i));
    }
  });
  const QString bin = directory.filePath(QStringLiteral("bin"));
  if (!QDir().mkpath(bin)) {
    error = QStringLiteral("Could not prepare email OCR PATH");
    return false;
  }
  const QByteArray script =
      "#!/bin/sh\n"
      "/bin/cat > \"$OMASNAP_EMAIL_TEST_DIR/input.png\"\n"
      "/bin/printf '%s\\n' \"$@\" > \"$OMASNAP_EMAIL_TEST_DIR/args\"\n"
      "/bin/printf '%s' \"$OMP_THREAD_LIMIT\" > \"$OMASNAP_EMAIL_TEST_DIR/omp\"\n"
      "case \"$OMASNAP_EMAIL_TEST_MODE\" in\n"
      "block) : > \"$OMASNAP_EMAIL_TEST_DIR/started\"; exec /bin/sleep 30;;\n"
      "fail) /bin/printf 'deliberate OCR failure' >&2; exit 4;;\n"
      "*) /bin/cat \"$OMASNAP_EMAIL_TEST_DIR/result.tsv\";;\n"
      "esac\n";
  const QByteArray clipboardScript =
      "#!/bin/sh\n"
      ": > \"$OMASNAP_EMAIL_TEST_DIR/clipboard-called\"\n"
      "exit 99\n";
  if (!writeFile(bin + QStringLiteral("/tesseract"), script, error, true) ||
      !writeFile(bin + QStringLiteral("/wl-copy"), clipboardScript, error, true) ||
      !writeFile(bin + QStringLiteral("/wl-paste"), clipboardScript, error, true))
    return false;
  qputenv("PATH", bin.toUtf8() + ':' + previous.first());
  qputenv("OMASNAP_EMAIL_TEST_DIR", directory.path().toUtf8());
  qputenv("OMASNAP_EMAIL_TEST_MODE", "normal");
  qputenv("OMASNAP_OCR_LANGS", "eng");
  qputenv("OMARCHY_OCR_LANGS", "eng");
  const QString resultsPath = directory.filePath(QStringLiteral("result.tsv"));
  const auto setResults = [&](const QString &tsv) {
    return writeFile(resultsPath, tsv.toUtf8(), error);
  };
  const auto stopped = std::make_shared<std::atomic_bool>(true);
  QString expectedError;
  if (!findEmailRedactions({}, QRectF(0, 0, 10, 10), {}, expectedError).isEmpty() ||
      expectedError.isEmpty() ||
      !findEmailRedactions(fixtureCapture(), QRectF(0, 0, 10, 10), {},
                            expectedError, stopped).isEmpty() ||
      !expectedError.contains(QStringLiteral("cancelled")) ||
      QFileInfo::exists(directory.filePath(QStringLiteral("input.png")))) {
    error = QStringLiteral("Invalid or pre-cancelled email input launched OCR");
    return false;
  }

  // Mapping is based on each source axis and the native crop's rounded origin,
  // including loaded documents whose source pixels must never be resampled.
  struct MappingCase { QSize native; QSize logical; QRectF selection; qreal scale; bool preserve; };
  const QList<MappingCase> cases{
      {QSize(320, 180), QSize(320, 180), QRectF(20, 25, 180, 110), 1, false},
      {QSize(640, 360), QSize(320, 180), QRectF(20, 25, 180, 110), 2, false},
      {QSize(501, 323), QSize(333, 211), QRectF(17.25, 29.45, 120.5, 79.25), 1.5, false},
      {QSize(501, 323), QSize(333, 211), QRectF(17.25, 29.45, 120.5, 79.25), 2, true}};
  const QVector<QRect> words{QRect(12, 10, 40, 14), QRect(0, 0, 8, 8)};
  if (!setResults(word(QStringLiteral("alice@example.com"), words.at(0)) +
                  word(QStringLiteral("bob@example.org"), words.at(1), 2)))
    return false;
  for (const MappingCase &test : cases) {
    CaptureData capture = fixtureCapture(test.native, test.logical);
    capture.monitor.scale = test.scale;
    capture.preserveSourceResolution = test.preserve;
    const QImage original = capture.source.copy();
    const Annotation redaction = solid(QRectF(55, 30, 18, 15));
    Annotation overlay;
    overlay.kind = Annotation::Kind::Rectangle;
    overlay.start = QPointF();
    overlay.end = QPointF(120, 70);
    overlay.color = Qt::red;
    overlay.filled = true;
    const auto regions = findEmailRedactions(capture, test.selection,
                                             {redaction, overlay}, error);
    const qreal sx = static_cast<qreal>(test.native.width()) / test.logical.width();
    const qreal sy = static_cast<qreal>(test.native.height()) / test.logical.height();
    const int left = static_cast<int>(std::floor(test.selection.x() * sx));
    const int top = static_cast<int>(std::floor(test.selection.y() * sy));
    const QSize cropSize(static_cast<int>(std::ceil(test.selection.right() * sx)) - left,
                         static_cast<int>(std::ceil(test.selection.bottom() * sy)) - top);
    if (!error.isEmpty() || regions.size() != words.size()) {
      error = QStringLiteral("Email native mapping did not return two boxes: %1").arg(error);
      return false;
    }
    for (qsizetype i = 0; i < words.size(); ++i) {
      const QRect padded = words.at(i).adjusted(-3, -3, 3, 3)
                               .intersected(QRect(QPoint(), cropSize));
      const QRectF expected = QRectF((left + padded.x()) / sx - test.selection.x(),
                                     (top + padded.y()) / sy - test.selection.y(),
                                     padded.width() / sx, padded.height() / sy)
                                 .intersected(QRectF(QPointF(), test.selection.size()));
      if (!nearRect(regions.at(i), expected)) {
        error = QStringLiteral("Email native box mapping lost axis scale or fractional crop origin");
        return false;
      }
    }
    CaptureData native = capture;
    native.preserveSourceResolution = true;
    const QImage expectedInput = renderCapture(native, test.selection, {redaction},
        BackgroundStyle::None, false, CanvasBoundaryMode::Image);
    const QImage actualInput(directory.filePath(QStringLiteral("input.png")));
    if (actualInput.convertToFormat(QImage::Format_ARGB32) !=
            expectedInput.convertToFormat(QImage::Format_ARGB32) ||
        actualInput.size() != cropSize || capture.source != original) {
      error = QStringLiteral("Email OCR did not receive native source plus only prior redactions");
      return false;
    }
    QVector<Annotation> suggestions;
    for (const QRectF &region : regions)
      suggestions.push_back(solid(region));
    const QImage exported = renderCapture(native, test.selection, suggestions,
        BackgroundStyle::None, false, CanvasBoundaryMode::Image);
    for (const QRect &wordBox : words) {
      for (int y = wordBox.top(); y <= wordBox.bottom(); ++y) {
        for (int x = wordBox.left(); x <= wordBox.right(); ++x) {
          if (exported.pixelColor(x, y) != QColor(QStringLiteral("#121216"))) {
            error = QStringLiteral("Email export retained an OCR word pixel after scaled crop mapping");
            return false;
          }
        }
      }
    }
  }
  const QByteArray args = readFile(directory.filePath(QStringLiteral("args")));
  if (!args.contains("stdin\n") || !args.contains("stdout\n") ||
      !args.contains("tsv\n") ||
      readFile(directory.filePath(QStringLiteral("omp"))) != "1") {
    error = QStringLiteral("Email OCR did not use in-memory TSV and a bounded Tesseract thread count");
    return false;
  }
  // Preserve the existing primary → Omarchy → English language precedence.
  for (const QByteArray &primary : {QByteArray("primary"), QByteArray()}) {
    qputenv("OMASNAP_OCR_LANGS", primary);
    qputenv("OMARCHY_OCR_LANGS", "fallback");
    QString languageError;
    static_cast<void>(findEmailRedactions(fixtureCapture(), QRectF(0, 0, 320, 180),
                                          {}, languageError));
    const QByteArray chosen = primary.isEmpty() ? QByteArray("fallback") : primary;
    if (!languageError.isEmpty() ||
        !readFile(directory.filePath(QStringLiteral("args"))).contains("-l\n" + chosen + '\n')) {
      error = QStringLiteral("Email OCR changed the established language precedence");
      return false;
    }
  }
  qunsetenv("OMASNAP_OCR_LANGS");
  qunsetenv("OMARCHY_OCR_LANGS");
  static_cast<void>(findEmailRedactions(fixtureCapture(), QRectF(0, 0, 320, 180),
                                        {}, expectedError));
  if (!expectedError.isEmpty() ||
      !readFile(directory.filePath(QStringLiteral("args"))).contains("-l\neng\n")) {
    error = QStringLiteral("Email OCR lost its English language fallback");
    return false;
  }
  qputenv("OMASNAP_OCR_LANGS", "eng");
  qputenv("OMARCHY_OCR_LANGS", "eng");

  const CaptureData capture = fixtureCapture();
  const auto prepare = [&](CaptureEditor &editor) {
    editor.setSuppressSnapshots(true);
    Operation crop;
    crop.type = Operation::Type::Crop;
    crop.crop = QRectF(QPointF(), capture.previewSize);
    editor.ops_ = {crop};
    editor.opIndex_ = 1;
    editor.nextAnnotationId_ = 1;
    editor.phase_ = CaptureEditor::Phase::Edit;
    editor.replayLog();
    editor.setWindowedPresentation(true);
    editor.resize(800, 600);
  };
  const auto scan = [&](CaptureEditor &editor) {
    editor.runEmailRedaction();
    if (!waitUntil([&] { return !editor.emailRedactionPending_; })) {
      error = QStringLiteral("Email redaction worker did not complete");
      return false;
    }
    return true;
  };
  const QString twoEmails = word(QStringLiteral("alice@example.com"), QRect(25, 25, 80, 20)) +
                            word(QStringLiteral("bob@example.org"), QRect(150, 95, 90, 20), 2);
  if (!setResults(twoEmails))
    return false;
  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
    prepare(editor);
    const int before = editor.operationIndex();
    if (!scan(editor) || editor.operationIndex() != before + 2 ||
        editor.annotationCountForTest() != 2) {
      error = QStringLiteral("Email scan did not append one operation per address: %1")
                  .arg(editor.statusForTest());
      return false;
    }
    for (const Annotation &annotation : editor.currentAnnotationsForTest()) {
      if (annotation.kind != Annotation::Kind::Redaction ||
          annotation.redactionStyle != RedactionStyle::Solid || annotation.id == 0) {
        error = QStringLiteral("Email scan did not create ordinary solid redaction layers");
        return false;
      }
    }
    const QImage rendered = editor.renderCurrentOutput();
    for (const QPoint &point : {QPoint(50, 35), QPoint(180, 105)}) {
      if (rendered.pixelColor(point) != QColor(QStringLiteral("#121216")) ||
          rendered.pixelColor(point).alpha() != 255) {
        error = QStringLiteral("Email export retained source pixels underneath a detected address");
        return false;
      }
    }
    if (rendered.pixelColor(5, 5) != capture.source.pixelColor(5, 5) ||
        editor.captureData().source != capture.source) {
      error = QStringLiteral("Email redaction changed unrelated or pristine pixels");
      return false;
    }
    editor.undoEdit();
    if (editor.annotationCountForTest() != 1 ||
        editor.renderCurrentOutput().pixelColor(180, 105) != capture.source.pixelColor(180, 105)) {
      error = QStringLiteral("Undo did not remove exactly one detected email redaction");
      return false;
    }
    editor.redoEdit();
    const QString logPath = directory.filePath(QStringLiteral("email-log.json"));
    if (editor.renderCurrentOutput() != rendered ||
        !saveOperationLog(logPath, editor.currentOperationLog(), error))
      return false;
    CaptureEditor restored(capture, CaptureEditor::CaptureMode::File);
    restored.setSuppressSnapshots(true);
    if (!restored.restoreOperationLog(logPath, error) ||
        restored.annotationCountForTest() != editor.annotationCountForTest() ||
        restored.renderCurrentOutput() != rendered) {
      error = QStringLiteral("Restoring the operation log lost email redactions");
      return false;
    }
    for (qsizetype i = 0; i < editor.currentAnnotationsForTest().size(); ++i) {
      const Annotation &expected = editor.currentAnnotationsForTest().at(i);
      const Annotation &actual = restored.currentAnnotationsForTest().at(i);
      if (actual.id != expected.id || actual.kind != Annotation::Kind::Redaction ||
          actual.redactionStyle != RedactionStyle::Solid ||
          actual.start != expected.start || actual.end != expected.end) {
        error = QStringLiteral("Restoring the operation log changed an email redaction's identity or geometry");
        return false;
      }
    }
  }

  // Refuse the entire result before the existing 100-operation history cap can
  // remove an older redaction. Include a result larger than the complete cap.
  for (const int detections : {2, 101}) {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
    prepare(editor);
    editor.commitAnnotate(solid(QRectF(5, 5, 15, 15)));
    while (editor.operationIndex() < (detections == 2 ? 99 : 2)) {
      Annotation annotation;
      annotation.kind = Annotation::Kind::Rectangle;
      annotation.start = QPointF(270, 145);
      annotation.end = QPointF(280, 155);
      annotation.color = Qt::green;
      editor.commitAnnotate(annotation);
    }
    QString tsv;
    for (int i = 0; i < detections; ++i)
      tsv += word(QStringLiteral("alice@example.com"), QRect(25 + i % 20, 25 + i / 20, 80, 20), i + 1);
    if (!setResults(tsv))
      return false;
    const OperationLog before = editor.currentOperationLog();
    const QImage output = editor.renderCurrentOutput();
    if (!scan(editor) || editor.currentOperationLog() != before ||
        editor.renderCurrentOutput() != output) {
      error = QStringLiteral("Email capacity refusal partially mutated or truncated history");
      return false;
    }
  }

  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
    prepare(editor);
    const OperationLog before = editor.currentOperationLog();
    if (!setResults(QString()) || !scan(editor) || editor.currentOperationLog() != before ||
        editor.busy_ || editor.statusForTest().isEmpty()) {
      error = QStringLiteral("An email scan with no matches changed the document or remained busy");
      return false;
    }
    qputenv("OMASNAP_EMAIL_TEST_MODE", "fail");
    if (!scan(editor) || editor.currentOperationLog() != before || editor.busy_ ||
        !editor.statusForTest().contains(QStringLiteral("failed"), Qt::CaseInsensitive)) {
      error = QStringLiteral("Failed email OCR changed the document or hid its failure");
      return false;
    }
    qputenv("OMASNAP_EMAIL_TEST_MODE", "normal");
  }

  // The worker sees the current composed source after a cut, rather than the
  // discarded band or pristine screenshot dimensions.
  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
    prepare(editor);
    editor.applyCutForTest(CutOp{Qt::Horizontal, 60, 80, 60, 80});
    if (!setResults(twoEmails) || !scan(editor))
      return false;
    const QImage input(directory.filePath(QStringLiteral("input.png")));
    if (input.size() != QSize(320, 160) ||
        input.pixelColor(2, 65) != capture.source.pixelColor(2, 85) ||
        editor.annotationCountForTest() != 2) {
      error = QStringLiteral("Email OCR did not use the source after the existing cut");
      return false;
    }
  }

  qputenv("OMASNAP_EMAIL_TEST_MODE", "block");
  {
    CaptureEditor editor(capture, CaptureEditor::CaptureMode::File);
    prepare(editor);
    editor.commitAnnotate(solid(QRectF(5, 5, 15, 15)));
    editor.selectedAnnotation_ = 0;
    editor.selectedAnnotations_ = {0};
    editor.show();
    QFile::remove(directory.filePath(QStringLiteral("started")));
    const OperationLog before = editor.currentOperationLog();
    editor.runEmailRedaction();
    if (!waitUntil([&] { return QFileInfo::exists(directory.filePath(QStringLiteral("started"))); }) ||
        !editor.busy_) {
      error = QStringLiteral("Blocking email fixture did not enter the asynchronous OCR process");
      return false;
    }
    bool heartbeat = false;
    QTimer::singleShot(0, &editor, [&] { heartbeat = true; });
    QTest::keyClick(&editor, Qt::Key_Delete);
    QTest::keyClick(&editor, Qt::Key_Right);
    QTest::keyClick(&editor, Qt::Key_Z, Qt::ControlModifier);
    if (!waitUntil([&] { return heartbeat; }) || editor.currentOperationLog() != before) {
      error = QStringLiteral("Email detection blocked GUI events or allowed a mutating shortcut");
      return false;
    }
    QElapsedTimer cancelTimer;
    cancelTimer.start();
    const auto stop = editor.emailRedactionStop_;
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(&editor, &escape);
    if (editor.busy_ || cancelTimer.elapsed() > 500 || !stop || !stop->load() ||
        !editor.emailRedactionPending_) {
      error = QStringLiteral("Escape did not promptly cancel email detection");
      return false;
    }
    // Completion is queued after Escape. It must not clear another job's busy
    // state or status, nor append results from the cancelled request.
    editor.busy_ = true;
    editor.setStatus(QStringLiteral("Later operation"));
    if (!waitUntil([&] { return !editor.emailRedactionPending_; }) ||
        editor.currentOperationLog() != before || !editor.busy_ ||
        editor.statusForTest() != QStringLiteral("Later operation")) {
      error = QStringLiteral("Cancelled email completion changed a later operation");
      return false;
    }
    editor.busy_ = false;
  }
  {
    auto editor = std::make_unique<CaptureEditor>(capture, CaptureEditor::CaptureMode::File);
    prepare(*editor);
    QFile::remove(directory.filePath(QStringLiteral("started")));
    editor->runEmailRedaction();
    if (!waitUntil([&] { return QFileInfo::exists(directory.filePath(QStringLiteral("started"))); })) {
      error = QStringLiteral("Email destruction fixture did not start");
      return false;
    }
    const auto stop = editor->emailRedactionStop_;
    QElapsedTimer destructionTimer;
    destructionTimer.start();
    editor.reset();
    if (!stop || !stop->load() || destructionTimer.elapsed() > 2000) {
      error = QStringLiteral("Editor destruction failed to cancel its own email OCR worker");
      return false;
    }
    QCoreApplication::processEvents();
  }
  if (QFileInfo::exists(directory.filePath(QStringLiteral("clipboard-called")))) {
    error = QStringLiteral("Email detection invoked a clipboard helper");
    return false;
  }

  // Real OCR is a separate integration smoke: output is checked geometrically
  // and by re-scanning the actual destroyed pixels, without timing thresholds.
  if (realTesseract.isEmpty()) {
    error = QStringLiteral("Email integration smoke requires the existing Tesseract dependency");
    return false;
  }
  qputenv("PATH", previous.first());
  qputenv("OMASNAP_EMAIL_TEST_MODE", "normal");
  CaptureData textCapture = fixtureCapture(QSize(1000, 260), QSize(1000, 260));
  textCapture.source.fill(Qt::white);
  {
    QPainter painter(&textCapture.source);
    QFont font(QStringLiteral("DejaVu Sans"));
    font.setPixelSize(44);
    painter.setFont(font);
    painter.setPen(Qt::black);
    painter.drawText(QPointF(35, 85), QStringLiteral("alice@example.com"));
    painter.drawText(QPointF(35, 185), QStringLiteral("bob@example.org"));
  }
  const QImage pristine = textCapture.source.copy();
  const QRectF selection(QPointF(), textCapture.previewSize);
  const auto regions = findEmailRedactions(textCapture, selection, {}, error);
  if (!error.isEmpty() || regions.size() != 2 ||
      !regions.at(0).intersects(QRectF(35, 45, 440, 55)) ||
      !regions.at(1).intersects(QRectF(35, 145, 440, 55))) {
    error = QStringLiteral("Real Tesseract did not detect the two synthetic English emails: %1 boxes; %2")
                .arg(regions.size()).arg(error);
    return false;
  }
  QVector<Annotation> redactions;
  for (const QRectF &region : regions)
    redactions.push_back(solid(region));
  const QImage rendered = renderCapture(textCapture, selection, redactions,
      BackgroundStyle::None, false, CanvasBoundaryMode::Image);
  if (!findEmailRedactions(textCapture, selection, redactions, error).isEmpty() ||
      !error.isEmpty() || textCapture.source != pristine) {
    error = QStringLiteral("Repeated real email OCR rediscovered already destroyed address pixels");
    return false;
  }
  for (const QRectF &region : regions) {
    const QColor pixel = rendered.pixelColor(region.center().toPoint());
    if (pixel != QColor(QStringLiteral("#121216")) || pixel.alpha() != 255) {
      error = QStringLiteral("Real email OCR export did not destroy detected pixels");
      return false;
    }
  }
  return true;
}
