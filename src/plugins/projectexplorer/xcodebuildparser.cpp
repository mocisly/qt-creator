// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "xcodebuildparser.h"

#include "projectexplorertr.h"
#include "task.h"

#include <utils/qtcassert.h>

#include <QCoreApplication>

#ifdef WITH_TESTS
#include "outputparser_test.h"
#include <QTest>
#endif

using namespace Utils;

namespace ProjectExplorer {

static const char failureRe[] = "\\*\\* BUILD FAILED \\*\\*$";
static const char successRe[] = "\\*\\* BUILD SUCCEEDED \\*\\*$";
static const char buildRe[] = "=== BUILD (AGGREGATE )?TARGET (.*) OF PROJECT (.*) WITH .* ===$";
static const char signatureChangeEndsWithPattern[] = ": replacing existing signature";

XcodebuildParser::XcodebuildParser()
    : m_failureRe(QLatin1String(failureRe))
    , m_successRe(QLatin1String(successRe))
    , m_buildRe(QLatin1String(buildRe))
{
    setObjectName(QLatin1String("XcodeParser"));
    QTC_CHECK(m_failureRe.isValid());
    QTC_CHECK(m_successRe.isValid());
    QTC_CHECK(m_buildRe.isValid());
}

OutputLineParser::Result XcodebuildParser::handleLine(const QString &line, OutputFormat type)
{
    static const QStringList notesPatterns({"note: Build preparation complete",
                                            "note: Building targets in parallel",
                                            "note: Planning build"});
    const QString lne = rightTrimmed(line);
    if (type == StdOutFormat) {
        QRegularExpressionMatch match = m_buildRe.match(line);
        if (match.hasMatch() || notesPatterns.contains(lne)) {
            m_xcodeBuildParserState = InXcodebuild;
            return Status::Done;
        }
        if (m_xcodeBuildParserState == InXcodebuild
                || m_xcodeBuildParserState == UnknownXcodebuildState) {
            match = m_successRe.match(lne);
            if (match.hasMatch()) {
                m_xcodeBuildParserState = OutsideXcodebuild;
                return Status::Done;
            }
            if (lne.endsWith(QLatin1String(signatureChangeEndsWithPattern))) {
                const int filePathEndPos = lne.size()
                        - QLatin1String(signatureChangeEndsWithPattern).size();
                CompileTask task(Task::Warning,
                                 Tr::tr("Replacing signature"),
                                 absoluteFilePath(FilePath::fromString(
                                     lne.left(filePathEndPos))));
                LinkSpecs linkSpecs;
                addLinkSpecForAbsoluteFilePath(linkSpecs, task.file(), task.line(), task.column(), 0,
                                               filePathEndPos);
                scheduleTask(task, 1);
                return {Status::Done, linkSpecs};
            }
        }
        return Status::NotHandled;
    }
    const QRegularExpressionMatch match = m_failureRe.match(lne);
    if (match.hasMatch()) {
        ++m_fatalErrorCount;
        m_xcodeBuildParserState = UnknownXcodebuildState;
        scheduleTask(CompileTask(Task::Error, Tr::tr("Xcodebuild failed.")), 1);
    }
    if (m_xcodeBuildParserState == OutsideXcodebuild)
        return Status::NotHandled;
    return Status::Done;
}

bool XcodebuildParser::hasDetectedRedirection() const
{
    return m_xcodeBuildParserState != OutsideXcodebuild;
}

} // namespace ProjectExplorer

// Unit tests:
#ifdef WITH_TESTS
Q_DECLARE_METATYPE(ProjectExplorer::XcodebuildParser::XcodebuildStatus)

namespace ProjectExplorer::Internal {

class XcodebuildParserTester : public QObject
{
public:
    explicit XcodebuildParserTester(XcodebuildParser *p, QObject *parent = nullptr) :
        QObject(parent),
        parser(p)
    { }

    XcodebuildParser *parser;
    XcodebuildParser::XcodebuildStatus expectedFinalState = XcodebuildParser::OutsideXcodebuild;

public:
    void onAboutToDeleteParser()
    {
        QCOMPARE(parser->m_xcodeBuildParserState, expectedFinalState);
    }
};

class XcodebuildParserTest : public QObject
{
    Q_OBJECT

private slots:
    void test_data()
    {
        QTest::addColumn<ProjectExplorer::XcodebuildParser::XcodebuildStatus>("initialStatus");
        QTest::addColumn<QString>("input");
        QTest::addColumn<OutputParserTester::Channel>("inputChannel");
        QTest::addColumn<QStringList>("childStdOutLines");
        QTest::addColumn<QStringList>("childStdErrLines");
        QTest::addColumn<Tasks>("tasks");
        QTest::addColumn<ProjectExplorer::XcodebuildParser::XcodebuildStatus>("finalStatus");

        QTest::newRow("outside pass-through stdout")
            << XcodebuildParser::OutsideXcodebuild
            << QString("Sometext") << OutputParserTester::STDOUT
            << QStringList("Sometext") << QStringList()
            << Tasks()
            << XcodebuildParser::OutsideXcodebuild;
        QTest::newRow("outside pass-through stderr")
            << XcodebuildParser::OutsideXcodebuild
            << QString::fromLatin1("Sometext") << OutputParserTester::STDERR
            << QStringList() << QStringList("Sometext")
            << Tasks()
            << XcodebuildParser::OutsideXcodebuild;
        QTest::newRow("inside pass stdout to stderr")
            << XcodebuildParser::InXcodebuild
            << QString::fromLatin1("Sometext") << OutputParserTester::STDOUT
            << QStringList() << QStringList("Sometext")
            << Tasks()
            << XcodebuildParser::InXcodebuild;
        QTest::newRow("inside ignore stderr")
            << XcodebuildParser::InXcodebuild
            << QString::fromLatin1("Sometext") << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << Tasks()
            << XcodebuildParser::InXcodebuild;
        QTest::newRow("unknown pass stdout to stderr")
            << XcodebuildParser::UnknownXcodebuildState
            << QString::fromLatin1("Sometext") << OutputParserTester::STDOUT
            << QStringList() << QStringList("Sometext")
            << Tasks()
            << XcodebuildParser::UnknownXcodebuildState;
        QTest::newRow("unknown ignore stderr (change?)")
            << XcodebuildParser::UnknownXcodebuildState
            << QString::fromLatin1("Sometext") << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << Tasks()
            << XcodebuildParser::UnknownXcodebuildState;
        QTest::newRow("switch outside->in->outside")
            << XcodebuildParser::OutsideXcodebuild
            << QString::fromLatin1("outside\n"
                                   "=== BUILD AGGREGATE TARGET Qt Preprocess OF PROJECT testQQ WITH THE DEFAULT CONFIGURATION (Debug) ===\n"
                                   "in xcodebuild\n"
                                   "=== BUILD TARGET testQQ OF PROJECT testQQ WITH THE DEFAULT CONFIGURATION (Debug) ===\n"
                                   "in xcodebuild2\n"
                                   "** BUILD SUCCEEDED **\n"
                                   "outside2")
            << OutputParserTester::STDOUT
            << QStringList{"outside","outside2"} << QStringList{"in xcodebuild", "in xcodebuild2"}
            << Tasks()
            << XcodebuildParser::OutsideXcodebuild;
        QTest::newRow("switch outside->in->outside (new)")
            << XcodebuildParser::OutsideXcodebuild
            << QString::fromLatin1("outside\n"
                                   "note: Build preparation complete\n"
                                   "in xcodebuild\n"
                                   "in xcodebuild2\n"
                                   "** BUILD SUCCEEDED **\n"
                                   "outside2")
            << OutputParserTester::STDOUT
            << QStringList{"outside", "outside2"} << QStringList{"in xcodebuild", "in xcodebuild2"}
            << Tasks()
            << XcodebuildParser::OutsideXcodebuild;
        QTest::newRow("switch Unknown->in->outside")
            << XcodebuildParser::UnknownXcodebuildState
            << QString::fromLatin1("unknown\n"
                                   "=== BUILD TARGET testQQ OF PROJECT testQQ WITH THE DEFAULT CONFIGURATION (Debug) ===\n"
                                   "in xcodebuild\n"
                                   "** BUILD SUCCEEDED **\n"
                                   "outside")
            << OutputParserTester::STDOUT
            << QStringList("outside") << QStringList{"unknown", "in xcodebuild"}
            << Tasks()
            << XcodebuildParser::OutsideXcodebuild;

        QTest::newRow("switch in->unknown")
            << XcodebuildParser::InXcodebuild
            << QString::fromLatin1("insideErr\n"
                                   "** BUILD FAILED **\n"
                                   "unknownErr")
            << OutputParserTester::STDERR
            << QStringList() << QStringList()
            << (Tasks()
                << CompileTask(Task::Error, Tr::tr("Xcodebuild failed.")))
            << XcodebuildParser::UnknownXcodebuildState;

        QTest::newRow("switch out->unknown")
            << XcodebuildParser::OutsideXcodebuild
            << QString::fromLatin1("outErr\n"
                                   "** BUILD FAILED **\n"
                                   "unknownErr")
            << OutputParserTester::STDERR
            << QStringList() << QStringList("outErr")
            << (Tasks()
                << CompileTask(Task::Error, Tr::tr("Xcodebuild failed.")))
            << XcodebuildParser::UnknownXcodebuildState;

        QTest::newRow("inside catch codesign replace signature")
            << XcodebuildParser::InXcodebuild
            << QString::fromLatin1("/somepath/somefile.app: replacing existing signature") << OutputParserTester::STDOUT
            << QStringList() << QStringList()
            << (Tasks()
                << CompileTask(Task::Warning,
                               Tr::tr("Replacing signature"), "/somepath/somefile.app"))
            << XcodebuildParser::InXcodebuild;

        QTest::newRow("outside forward codesign replace signature")
            << XcodebuildParser::OutsideXcodebuild
            << QString::fromLatin1("/somepath/somefile.app: replacing existing signature") << OutputParserTester::STDOUT
            << QStringList("/somepath/somefile.app: replacing existing signature") << QStringList()
            << Tasks()
            << XcodebuildParser::OutsideXcodebuild;
    }

    void test()
    {
        OutputParserTester testbench;
        auto *childParser = new XcodebuildParser;
        auto *tester = new XcodebuildParserTester(childParser);

        connect(&testbench, &OutputParserTester::aboutToDeleteParser,
                tester, &XcodebuildParserTester::onAboutToDeleteParser);

        testbench.addLineParser(childParser);
        QFETCH(ProjectExplorer::XcodebuildParser::XcodebuildStatus, initialStatus);
        QFETCH(QString, input);
        QFETCH(OutputParserTester::Channel, inputChannel);
        QFETCH(QStringList, childStdOutLines);
        QFETCH(QStringList, childStdErrLines);
        QFETCH(Tasks, tasks);
        QFETCH(ProjectExplorer::XcodebuildParser::XcodebuildStatus, finalStatus);

        tester->expectedFinalState = finalStatus;
        childParser->m_xcodeBuildParserState = initialStatus;
        testbench.testParsing(input, inputChannel, tasks, childStdOutLines, childStdErrLines);

        delete tester;
    }
};

QObject *createXcodebuildParserTest()
{
    return new XcodebuildParserTest;
}

} // namespace ProjectExplorer::Internal

#include <xcodebuildparser.moc>
#endif // WITH_TESTS
