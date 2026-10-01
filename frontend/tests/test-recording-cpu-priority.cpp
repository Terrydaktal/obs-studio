#include <utility/RecordingCpuPriority.hpp>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDBusVirtualObject>
#include <QElapsedTimer>
#include <QFile>
#include <QMap>
#include <QThread>
#include <QTimer>

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <unistd.h>

using Property = std::pair<QString, QDBusVariant>;
using Properties = QList<Property>;

static void Check(bool condition, const char *message)
{
	if (!condition) {
		throw std::runtime_error(message);
	}
}

static void WaitUntil(const std::function<bool()> &condition, const char *message)
{
	QElapsedTimer elapsed;
	elapsed.start();
	QElapsedTimer stable;
	while (elapsed.elapsed() < 7000) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
		if (!condition()) {
			stable.invalidate();
		} else if (!stable.isValid()) {
			stable.start();
		} else if (stable.elapsed() >= 100) {
			return;
		}
		QThread::msleep(5);
	}
	Check(false, message);
}

class FakeManager : public QDBusVirtualObject {
public:
	QMap<QString, qulonglong> weights;
	QString latestScope;
	int creates = 0;
	int updates = 0;
	int outstandingReplies = 0;
	int deniedUpdates = 0;
	int deniedCreates = 0;
	bool loseCreateReply = false;
	int replyDelayMs = 40;

	QString introspect(const QString &) const override { return {}; }

	bool handleMessage(const QDBusMessage &message, const QDBusConnection &connection) override
	{
		const bool creating = message.member() == QStringLiteral("StartTransientUnit");
		if (!creating && message.member() != QStringLiteral("SetUnitProperties")) {
			return false;
		}
		const auto arguments = message.arguments();
		Check(message.signature() == (creating ? "ssa(sv)a(sa(sv))" : "sba(sv)"), "D-Bus signature mismatch");
		const QString scope = arguments[0].toString();
		Check(scope.startsWith(QStringLiteral("app-obs-recording-%1-").arg(QCoreApplication::applicationPid())),
		      "must target our own uniquely named scope");
		Check(scope.endsWith(QStringLiteral(".scope")), "must create a scope");
		QMap<QString, QVariant> properties;
		for (const auto &[name, value] : qdbus_cast<Properties>(arguments[2])) {
			properties.insert(name, value.variant());
		}
		const auto weight = properties.value(QStringLiteral("CPUWeight")).toULongLong();
		Check(weight == 100 || weight == 1000, "unexpected CPU weight");

		QString error;
		if (creating) {
			++creates;
			latestScope = scope;
			Check(arguments[1].toString() == "fail", "must not replace unrelated units");
			Check(properties.value("Slice").toString() == "app.slice", "unexpected slice");
			Check(properties.value("CollectMode").toString() == "inactive-or-failed",
			      "scope must auto-collect");
			const auto pids = qdbus_cast<QList<quint32>>(properties.value("PIDs"));
			Check(pids == QList<quint32>{quint32(QCoreApplication::applicationPid())},
			      "must move only this PID");
			if (deniedCreates > 0) {
				--deniedCreates;
				error = "org.freedesktop.DBus.Error.AccessDenied";
			} else if (weights.contains(scope)) {
				error = "org.freedesktop.systemd1.UnitExists";
			}
		} else {
			++updates;
			Check(arguments[1].toBool(), "properties must be runtime-only");
			Check(properties.size() == 1, "must change only CPUWeight");
			if (deniedUpdates > 0) {
				--deniedUpdates;
				error = "org.freedesktop.DBus.Error.AccessDenied";
			} else if (!weights.contains(scope)) {
				error = "org.freedesktop.systemd1.NoSuchUnit";
			}
		}
		if (error.isEmpty()) {
			weights[scope] = weight;
			if (creating && loseCreateReply) {
				loseCreateReply = false;
				error = "org.freedesktop.DBus.Error.NoReply";
			}
		}
		auto reply = error.isEmpty() ? message.createReply()
					     : message.createErrorReply(error, "injected test failure");
		if (creating && error.isEmpty()) {
			reply.setArguments({QVariant::fromValue(
				QDBusObjectPath(QStringLiteral("/org/freedesktop/systemd1/job/1")))});
		}
		++outstandingReplies;
		QTimer::singleShot(replyDelayMs, this, [this, reply, connection] {
			connection.send(reply);
			--outstandingReplies;
		});
		return true;
	}

	bool AtWeight(qulonglong weight) const
	{
		return weights.contains(latestScope) && weights[latestScope] == weight && !outstandingReplies;
	}
};

static void TestStateTransitions()
{
	auto connection = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("test-manager"));
	FakeManager manager;
	Check(connection.registerVirtualObject(QStringLiteral("/org/freedesktop/systemd1"), &manager),
	      "register manager");
	Check(connection.registerService(QStringLiteral("org.freedesktop.systemd1")), "use a private test bus");

	{
		RecordingCpuPriority priority;
		priority.SetRecording(false);
		Check(manager.creates == 0, "idle OBS must not create a scope");
		priority.SetRecording(true);
		WaitUntil([&] { return manager.AtWeight(1000); }, "initial boost");
		priority.SetRecording(true);
		priority.SetRecording(false);
		WaitUntil([&] { return manager.AtWeight(100); }, "normal stop must restore weight");
		Check(manager.creates == 1, "repeated starts must reuse the same scope");

		priority.SetRecording(true);
		priority.SetRecording(false);
		WaitUntil([&] { return manager.AtWeight(100) && manager.updates >= 3; },
			  "in-flight boost must be undone");
		priority.SetRecording(true);
		WaitUntil([&] { return manager.AtWeight(1000); }, "restart boost");
		priority.SetRecording(false);
		priority.SetRecording(true);
		WaitUntil([&] { return manager.AtWeight(1000) && manager.updates >= 6; },
			  "in-flight restore must be reboosted");

		manager.deniedUpdates = 1;
		priority.SetRecording(false);
		WaitUntil([&] { return manager.AtWeight(100); }, "failed restore must retry");
		priority.SetRecording(true);
		WaitUntil([&] { return manager.AtWeight(1000); }, "boost before destruction");
	}
	WaitUntil([&] { return manager.AtWeight(100); }, "destructor must restore weight");

	{
		RecordingCpuPriority priority;
		const int creates = manager.creates;
		priority.SetRecording(true);
		priority.SetRecording(false);
		WaitUntil([&] { return manager.creates > creates && manager.AtWeight(100); },
			  "failed start during scope creation");
	}

	{
		RecordingCpuPriority priority;
		manager.loseCreateReply = true;
		priority.SetRecording(true);
		const int creates = manager.creates;
		WaitUntil([&] { return manager.creates >= creates + 2 && manager.AtWeight(1000); },
			  "recover uncertain create");
		priority.SetRecording(false);
		WaitUntil([&] { return manager.AtWeight(100); }, "restore after uncertain create");
	}

	{
		RecordingCpuPriority priority;
		manager.deniedCreates = 3;
		const int creates = manager.creates;
		QElapsedTimer elapsed;
		elapsed.start();
		priority.SetRecording(true);
		Check(elapsed.elapsed() < 100, "D-Bus failures must not block recording startup");
		WaitUntil([&] { return manager.creates >= creates + 3 && !manager.outstandingReplies; },
			  "bounded failure retries");
		priority.SetRecording(false);
		WaitUntil([&] { return !manager.outstandingReplies; }, "stop with no scope");
		priority.SetRecording(true);
		WaitUntil([&] { return manager.AtWeight(1000); }, "next recording must retry an unavailable boost");
	}
	WaitUntil([&] { return manager.AtWeight(100); }, "final destructor restore");
	connection.unregisterService(QStringLiteral("org.freedesktop.systemd1"));
	connection.unregisterObject(QStringLiteral("/org/freedesktop/systemd1"));
}

static QByteArray ReadFile(const QString &path)
{
	QFile file(path);
	Check(file.open(QIODevice::ReadOnly), "open cgroup file");
	return file.readAll().trimmed();
}

static QString CgroupPath(qint64 pid)
{
	const auto entries = ReadFile(QStringLiteral("/proc/%1/cgroup").arg(pid)).split('\n');
	for (const auto &entry : entries) {
		if (entry.startsWith("0::")) {
			return QStringLiteral("/sys/fs/cgroup") + QString::fromUtf8(entry.mid(3));
		}
	}
	throw std::runtime_error("cgroup v2 is required for the opt-in systemd test");
}

static void TestRealSystemd()
{
	const auto parentPath = CgroupPath(getppid());
	// systemd may not have enabled the CPU controller for app.slice yet.
	// Its unconfigured children have the default weight of 100 in that case.
	const auto parentWeight = QFile::exists(parentPath + "/cpu.weight") ? ReadFile(parentPath + "/cpu.weight")
									    : QByteArray("100");
	const auto pid = QCoreApplication::applicationPid();
	const auto scopePrefix = QStringLiteral("/app-obs-recording-%1-").arg(pid);
	auto atWeight = [&](const QByteArray &weight) {
		const auto path = CgroupPath(pid);
		return path.contains(scopePrefix) && ReadFile(path + "/cpu.weight") == weight;
	};
	{
		RecordingCpuPriority priority;
		priority.SetRecording(true);
		WaitUntil([&] { return atWeight("1000"); }, "actual kernel CPU weight must become 1000");
		Check(ReadFile(CgroupPath(pid) + "/cgroup.procs") == QByteArray::number(pid),
		      "scope must contain only test PID");
		Check(CgroupPath(getppid()) == parentPath && ReadFile(parentPath + "/cpu.weight") == parentWeight,
		      "launcher must keep its original cgroup and weight");
		priority.SetRecording(false);
		WaitUntil([&] { return atWeight("100"); }, "actual kernel CPU weight must restore to 100");
		priority.SetRecording(true);
		WaitUntil([&] { return atWeight("1000"); }, "actual kernel CPU weight must boost again");
	}
	WaitUntil([&] { return atWeight("100"); }, "destruction must restore actual CPU weight");
}

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	try {
		if (app.arguments().contains(QStringLiteral("--systemd"))) {
			TestRealSystemd();
		} else {
			TestStateTransitions();
		}
		std::puts("Recording CPU priority tests passed.");
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
