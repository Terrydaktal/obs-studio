#include "RecordingCpuPriority.hpp"

#ifdef __linux__
#include <util/base.h>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QTimer>
#include <QUuid>

#include <optional>
#include <utility>

namespace {
using Property = std::pair<QString, QDBusVariant>;
using Properties = QList<Property>;
using AuxiliaryUnit = std::pair<QString, Properties>;
using AuxiliaryUnits = QList<AuxiliaryUnit>;

constexpr qulonglong NORMAL_WEIGHT = 100;
constexpr qulonglong RECORDING_WEIGHT = 1000;
constexpr int CALL_TIMEOUT_MS = 1500;
constexpr int RETRY_INTERVAL_MS = 2000;
constexpr int MAX_FAILURES = 3;

QDBusMessage ManagerCall(const char *method)
{
	return QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
					      QStringLiteral("/org/freedesktop/systemd1"),
					      QStringLiteral("org.freedesktop.systemd1.Manager"),
					      QString::fromLatin1(method));
}
} // namespace

struct RecordingCpuPriority::Impl : QObject {
	QDBusConnection bus = QDBusConnection::sessionBus();
	QString scope;
	QTimer retryTimer;
	bool recording = false;
	bool attempted = false;
	bool created = false;
	bool pending = false;
	int failures = 0;
	std::optional<qulonglong> appliedWeight;

	Impl()
	{
		qDBusRegisterMetaType<Property>();
		qDBusRegisterMetaType<Properties>();
		qDBusRegisterMetaType<AuxiliaryUnit>();
		qDBusRegisterMetaType<AuxiliaryUnits>();
		qDBusRegisterMetaType<QList<quint32>>();
		retryTimer.setSingleShot(true);
		connect(&retryTimer, &QTimer::timeout, this, [this] { Apply(); });
	}

	~Impl() override
	{
		// Also covers shutdown with a D-Bus request in flight. Messages on this
		// connection are ordered, so this follows any outstanding boost request.
		// Never StopUnit: that would terminate OBS and its child processes.
		if (attempted) {
			bus.send(WeightCall(NORMAL_WEIGHT));
		}
	}

	qulonglong TargetWeight() const { return recording ? RECORDING_WEIGHT : NORMAL_WEIGHT; }

	QDBusMessage WeightCall(qulonglong weight) const
	{
		auto message = ManagerCall("SetUnitProperties");
		Properties properties{{QStringLiteral("CPUWeight"), QDBusVariant(QVariant::fromValue(weight))}};
		message.setArguments({scope, true, QVariant::fromValue(properties)});
		return message;
	}

	void SetRecording(bool active)
	{
		if (recording != active) {
			recording = active;
			failures = 0;
			retryTimer.stop();
		}
		Apply();
	}

	void Apply()
	{
		if (pending || retryTimer.isActive() || failures >= MAX_FAILURES || (!recording && !attempted)) {
			return;
		}
		const auto weight = TargetWeight();
		if (appliedWeight == weight) {
			return;
		}

		if (scope.isEmpty()) {
			scope = QStringLiteral("app-obs-recording-%1-%2.scope")
					.arg(QCoreApplication::applicationPid())
					.arg(QUuid::createUuid().toString(QUuid::Id128));
		}

		const bool creating = recording && !created;
		auto message = WeightCall(weight);
		if (creating) {
			// Do not change the inherited scope: a terminal-launched OBS may
			// share it with the shell and unrelated jobs. Move only our PID
			// (including all its threads) to our own transient user scope.
			Properties properties{
				{QStringLiteral("Description"),
				 QDBusVariant(QStringLiteral("OBS Studio recording CPU priority"))},
				{QStringLiteral("Slice"), QDBusVariant(QStringLiteral("app.slice"))},
				{QStringLiteral("PIDs"), QDBusVariant(QVariant::fromValue(QList<quint32>{
								 quint32(QCoreApplication::applicationPid())}))},
				{QStringLiteral("CPUWeight"), QDBusVariant(QVariant::fromValue(weight))},
				{QStringLiteral("CollectMode"), QDBusVariant(QStringLiteral("inactive-or-failed"))},
			};
			message = ManagerCall("StartTransientUnit");
			message.setArguments({scope, QStringLiteral("fail"), QVariant::fromValue(properties),
					      QVariant::fromValue(AuxiliaryUnits{})});
		}

		attempted = true;
		pending = true;
		auto watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, CALL_TIMEOUT_MS), this);
		connect(watcher, &QDBusPendingCallWatcher::finished, this,
			[this, creating, weight](QDBusPendingCallWatcher *finished) {
				const auto error = finished->error();
				finished->deleteLater();
				pending = false;

				// A timed-out create may still have succeeded on the manager.
				// Our unique name makes UnitExists safe to recover by setting
				// the desired weight, including when Stop raced with creation.
				if (creating && error.name() == QStringLiteral("org.freedesktop.systemd1.UnitExists")) {
					created = true;
					appliedWeight.reset();
					Apply();
					return;
				}

				if (!creating &&
				    error.name() == QStringLiteral("org.freedesktop.systemd1.NoSuchUnit")) {
					created = false;
					if (!recording) {
						appliedWeight = NORMAL_WEIGHT;
						return;
					}
				}

				if (error.isValid()) {
					appliedWeight.reset();
					++failures;
					blog(LOG_WARNING,
					     "[recording CPU priority] Could not %s CPU weight for %s: %s: %s "
					     "(attempt %d/%d). Recording is not blocked.",
					     weight == NORMAL_WEIGHT ? "restore" : "boost", scope.toUtf8().constData(),
					     error.name().toUtf8().constData(), error.message().toUtf8().constData(),
					     failures, MAX_FAILURES);
					if (weight != TargetWeight()) {
						Apply();
					} else if (failures < MAX_FAILURES) {
						retryTimer.start(RETRY_INTERVAL_MS);
					}
					return;
				}

				created = true;
				failures = 0;
				appliedWeight = weight;
				blog(LOG_INFO, "[recording CPU priority] Requested CPU weight %llu for %s (%s).",
				     weight, scope.toUtf8().constData(),
				     weight == NORMAL_WEIGHT ? "normal" : "recording, including encoder drain");
				// Coalesce Start/Stop changes while a request was outstanding.
				Apply();
			});
	}
};
#else
struct RecordingCpuPriority::Impl {
	void SetRecording(bool) {}
};
#endif

RecordingCpuPriority::RecordingCpuPriority() : impl(std::make_unique<Impl>()) {}
RecordingCpuPriority::~RecordingCpuPriority() = default;

void RecordingCpuPriority::SetRecording(bool active)
{
	impl->SetRecording(active);
}
