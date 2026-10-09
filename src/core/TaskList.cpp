#include "TaskList.hpp"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <QVariantMap>

namespace
{
	const char *const	FileName = "tasks.json";
	constexpr int		FormatVersion = 1;
}

TaskList::TaskList(QObject *parent)
	: QAbstractListModel(parent)
{
	_saveTimer.setSingleShot(true);
	_saveTimer.setInterval(SaveDelayMs);

	connect(&_saveTimer, &QTimer::timeout, this, &TaskList::save);

	load();
}

TaskList::~TaskList()
{
	if (_saveTimer.isActive())
	{
		_saveTimer.stop();
		save();
	}
}

int	TaskList::rowCount(const QModelIndex &parent) const
{
	return parent.isValid() ? 0 : static_cast<int>(_tasks.size());
}

QVariant	TaskList::data(const QModelIndex &index, int role) const
{
	if (!index.isValid() || !validRow(index.row()))
		return QVariant();

	const Task	&task = _tasks.at(index.row());

	switch (role)
	{
		case TaskIdRole:
			return task.id;
		case Qt::DisplayRole:
		case TitleRole:
			return task.title;
		case DoneRole:
			return task.done;
		case EstimateRole:
			return task.estimate;
		case CompletedRole:
			return task.completed;
		case ActiveRole:
			return task.id == _activeId;
		case NotesRole:
			return task.notes;
		case StepsRole:
			return stepList(task);
		case StepCountRole:
			return static_cast<int>(task.steps.size());
		case StepsDoneRole:
			return task.stepsDone();
		default:
			return QVariant();
	}
}

QHash<int, QByteArray>	TaskList::roleNames() const
{
	return {
		{ TaskIdRole, "taskId" },
		{ TitleRole, "title" },
		{ DoneRole, "done" },
		{ EstimateRole, "estimate" },
		{ CompletedRole, "completed" },
		{ ActiveRole, "active" },
		{ NotesRole, "notes" },
		{ StepsRole, "steps" },
		{ StepCountRole, "stepCount" },
		{ StepsDoneRole, "stepsDone" }
	};
}

int	TaskList::count() const
{
	return static_cast<int>(_tasks.size());
}

int	TaskList::openCount() const
{
	int	open = 0;

	for (const Task &task : _tasks)
	{
		if (!task.done)
			open++;
	}

	return open;
}

QString	TaskList::activeTaskId() const
{
	return _activeId;
}

QString	TaskList::activeTitle() const
{
	int	row = rowOf(_activeId);

	return row < 0 ? QString() : _tasks.at(row).title;
}

int	TaskList::activeCompleted() const
{
	int	row = rowOf(_activeId);

	return row < 0 ? 0 : _tasks.at(row).completed;
}

int	TaskList::activeEstimate() const
{
	int	row = rowOf(_activeId);

	return row < 0 ? 0 : _tasks.at(row).estimate;
}

int	TaskList::activeStepCount() const
{
	int	row = rowOf(_activeId);

	return row < 0 ? 0 : static_cast<int>(_tasks.at(row).steps.size());
}

int	TaskList::activeStepsDone() const
{
	int	row = rowOf(_activeId);

	return row < 0 ? 0 : _tasks.at(row).stepsDone();
}

bool	TaskList::add(const QString &title, int estimate)
{
	QString	trimmed = title.simplified().left(MaximumTitleLength);

	if (trimmed.isEmpty())
		return false;

	Task	task;

	task.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	task.title = trimmed;
	task.estimate = qBound(1, estimate, MaximumEstimate);

	int	row = static_cast<int>(_tasks.size());

	beginInsertRows(QModelIndex(), row, row);
	_tasks.append(task);
	endInsertRows();

	emit countChanged();

	// The first task of an empty list is almost certainly the one about to be worked on,
	// so it is picked for the user rather than making them click it as well.
	if (_activeId.isEmpty())
		setActiveId(task.id);

	touch();
	return true;
}

void	TaskList::remove(int row)
{
	if (!validRow(row))
		return;

	bool	wasActive = _tasks.at(row).id == _activeId;

	beginRemoveRows(QModelIndex(), row, row);
	_tasks.removeAt(row);
	endRemoveRows();

	emit countChanged();

	if (wasActive)
		setActiveId(QString());

	touch();
}

void	TaskList::rename(int row, const QString &title)
{
	if (!validRow(row))
		return;

	QString	trimmed = title.simplified().left(MaximumTitleLength);

	// Clearing a title is not a way to delete a task; the old one stays.
	if (trimmed.isEmpty() || trimmed == _tasks.at(row).title)
		return;

	_tasks[row].title = trimmed;
	changed(row, { TitleRole });

	if (_tasks.at(row).id == _activeId)
		emit activeChanged();

	touch();
}

void	TaskList::setDone(int row, bool done)
{
	if (!validRow(row) || _tasks.at(row).done == done)
		return;

	_tasks[row].done = done;
	changed(row, { DoneRole });

	emit countChanged();

	// A finished task stops collecting sessions.
	if (done && _tasks.at(row).id == _activeId)
		setActiveId(QString());

	touch();
}

void	TaskList::setEstimate(int row, int estimate)
{
	estimate = qBound(1, estimate, MaximumEstimate);

	if (!validRow(row) || _tasks.at(row).estimate == estimate)
		return;

	_tasks[row].estimate = estimate;
	changed(row, { EstimateRole });

	if (_tasks.at(row).id == _activeId)
		emit activeChanged();

	touch();
}

void	TaskList::setNotes(int row, const QString &notes)
{
	if (!validRow(row))
		return;

	QString	cleaned = cleanNotes(notes);

	if (cleaned == _tasks.at(row).notes)
		return;

	_tasks[row].notes = cleaned;
	changed(row, { NotesRole });
	touch();
}

bool	TaskList::addStep(int row, const QString &text)
{
	QString	trimmed = text.simplified().left(MaximumStepLength);

	if (!validRow(row) || trimmed.isEmpty() || _tasks.at(row).steps.size() >= MaximumSteps)
		return false;

	_tasks[row].steps.append(Step{ trimmed, false });
	stepsChanged(row);
	return true;
}

void	TaskList::renameStep(int row, int step, const QString &text)
{
	QString	trimmed = text.simplified().left(MaximumStepLength);

	// As with a task's title, clearing a step's text does not delete it.
	if (!validStep(row, step) || trimmed.isEmpty() || trimmed == _tasks.at(row).steps.at(step).text)
		return;

	_tasks[row].steps[step].text = trimmed;
	stepsChanged(row);
}

void	TaskList::setStepDone(int row, int step, bool done)
{
	if (!validStep(row, step) || _tasks.at(row).steps.at(step).done == done)
		return;

	_tasks[row].steps[step].done = done;
	stepsChanged(row);
}

void	TaskList::removeStep(int row, int step)
{
	if (!validStep(row, step))
		return;

	_tasks[row].steps.removeAt(step);
	stepsChanged(row);
}

void	TaskList::toggleActive(int row)
{
	if (!validRow(row))
		return;

	const Task	&task = _tasks.at(row);

	if (task.id == _activeId)
	{
		setActiveId(QString());
		return;
	}

	// Picking a finished task to work on reopens it.
	if (task.done)
		setDone(row, false);

	setActiveId(_tasks.at(row).id);
}

void	TaskList::clearDone()
{
	bool	removed = false;

	for (int row = static_cast<int>(_tasks.size()) - 1; row >= 0; row--)
	{
		if (!_tasks.at(row).done)
			continue;

		beginRemoveRows(QModelIndex(), row, row);
		_tasks.removeAt(row);
		endRemoveRows();

		removed = true;
	}

	if (!removed)
		return;

	emit countChanged();
	touch();
}

QString	TaskList::creditFocusSession()
{
	int	row = rowOf(_activeId);

	if (row < 0)
		return QString();

	_tasks[row].completed++;
	changed(row, { CompletedRole });

	emit activeChanged();
	touch();

	return _activeId;
}

int	TaskList::rowOf(const QString &id) const
{
	if (id.isEmpty())
		return -1;

	for (int row = 0; row < _tasks.size(); row++)
	{
		if (_tasks.at(row).id == id)
			return row;
	}

	return -1;
}

bool	TaskList::validRow(int row) const
{
	return row >= 0 && row < _tasks.size();
}

bool	TaskList::validStep(int row, int step) const
{
	return validRow(row) && step >= 0 && step < _tasks.at(row).steps.size();
}

// A step's change moves the task's counts too, and the home screen shows them for the
// active task.
void	TaskList::stepsChanged(int row)
{
	changed(row, { StepsRole, StepCountRole, StepsDoneRole });

	if (_tasks.at(row).id == _activeId)
		emit activeChanged();

	touch();
}

QVariantList	TaskList::stepList(const Task &task)
{
	QVariantList	list;

	for (const Step &step : task.steps)
		list.append(QVariantMap{ { QStringLiteral("text"), step.text }, { QStringLiteral("done"), step.done } });

	return list;
}

// Unlike a title, a description is free text: its lines are kept, only the blank space
// around it is dropped.
QString	TaskList::cleanNotes(const QString &notes)
{
	return notes.trimmed().left(MaximumNotesLength);
}

int	TaskList::Task::stepsDone() const
{
	int	done = 0;

	for (const Step &step : steps)
	{
		if (step.done)
			done++;
	}

	return done;
}

void	TaskList::changed(int row, const QList<int> &roles)
{
	QModelIndex	at = index(row);

	emit dataChanged(at, at, roles);
}

void	TaskList::setActiveId(const QString &id)
{
	if (_activeId == id)
		return;

	int	previous = rowOf(_activeId);

	_activeId = id;

	if (previous >= 0)
		changed(previous, { ActiveRole });

	int	current = rowOf(_activeId);

	if (current >= 0)
		changed(current, { ActiveRole });

	emit activeChanged();
	touch();
}

void	TaskList::touch()
{
	_saveTimer.start();
}

QString	TaskList::storagePath() const
{
	QString	directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

	return directory + QLatin1Char('/') + QLatin1String(FileName);
}

void	TaskList::load()
{
	QFile	file(storagePath());

	if (!file.exists())
		return;

	if (!file.open(QIODevice::ReadOnly))
	{
		qWarning() << "pomodoro: could not read" << file.fileName() << file.errorString();
		return;
	}

	QJsonParseError	error;
	QJsonDocument	document = QJsonDocument::fromJson(file.readAll(), &error);

	if (error.error != QJsonParseError::NoError || !document.isObject())
	{
		qWarning() << "pomodoro: task list is not valid JSON:" << error.errorString();
		return;
	}

	QJsonObject	root = document.object();
	QJsonArray	array = root.value(QStringLiteral("tasks")).toArray();

	for (const QJsonValue &value : array)
	{
		if (!value.isObject())
			continue;

		QJsonObject	object = value.toObject();
		Task		task;

		task.id = object.value(QStringLiteral("id")).toString();
		task.title = object.value(QStringLiteral("title")).toString().simplified().left(MaximumTitleLength);

		// A row without an id or a title is not something the user could have made.
		if (task.id.isEmpty() || task.title.isEmpty() || rowOf(task.id) >= 0)
			continue;

		task.done = object.value(QStringLiteral("done")).toBool();
		task.estimate = qBound(1, object.value(QStringLiteral("estimate")).toInt(1), MaximumEstimate);
		task.completed = qMax(0, object.value(QStringLiteral("completed")).toInt());
		task.notes = cleanNotes(object.value(QStringLiteral("notes")).toString());

		// Missing in files written before tasks had descriptions and steps.
		for (const QJsonValue &stepValue : object.value(QStringLiteral("steps")).toArray())
		{
			QJsonObject	stepObject = stepValue.toObject();
			QString		text = stepObject.value(QStringLiteral("text")).toString().simplified().left(MaximumStepLength);

			if (text.isEmpty() || task.steps.size() >= MaximumSteps)
				continue;

			task.steps.append(Step{ text, stepObject.value(QStringLiteral("done")).toBool() });
		}

		_tasks.append(task);
	}

	QString	active = root.value(QStringLiteral("activeTaskId")).toString();

	if (rowOf(active) >= 0)
		_activeId = active;
}

void	TaskList::save()
{
	QString error = writeFile(storagePath());
	if (!error.isEmpty())
		qWarning() << "pomodoro:" << error;
}

QString TaskList::exportFile(const QUrl &url) const
{
	if (!url.isLocalFile())
		return QStringLiteral("Choose a file on this computer.");
	return writeFile(url.toLocalFile());
}

QString TaskList::importFile(const QUrl &url)
{
	if (!url.isLocalFile())
		return QStringLiteral("Choose a file on this computer.");
	QFile file(url.toLocalFile());
	if (!file.open(QIODevice::ReadOnly))
		return file.errorString();
	if (file.size() > 10 * 1024 * 1024)
		return QStringLiteral("The task file is too large (maximum 10 MB).");
	QJsonParseError error;
	const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
	const QJsonObject root = document.object();
	if (error.error != QJsonParseError::NoError || !document.isObject()
		|| root.value(QStringLiteral("version")).toInt(-1) != FormatVersion
		|| !root.value(QStringLiteral("tasks")).isArray()
		|| !root.value(QStringLiteral("activeTaskId")).isString())
		return QStringLiteral("This is not a supported Pomodoro task file.");

	QVector<Task> imported;
	QSet<QString> ids;
	for (const QJsonValue &value : root.value(QStringLiteral("tasks")).toArray())
	{
		const QJsonObject object = value.toObject();
		Task task;
		task.id = object.value(QStringLiteral("id")).toString();
		task.title = object.value(QStringLiteral("title")).toString().simplified();
		task.estimate = object.value(QStringLiteral("estimate")).toInt(-1);
		task.completed = object.value(QStringLiteral("completed")).toInt(-1);
		if (!value.isObject() || task.id.isEmpty() || ids.contains(task.id)
			|| task.title.isEmpty() || task.title.size() > MaximumTitleLength
			|| !object.value(QStringLiteral("done")).isBool()
			|| task.estimate < 1 || task.estimate > MaximumEstimate || task.completed < 0
			|| (object.contains(QStringLiteral("notes")) && !object.value(QStringLiteral("notes")).isString())
			|| (object.contains(QStringLiteral("steps")) && !object.value(QStringLiteral("steps")).isArray()))
			return QStringLiteral("The task file contains an invalid or duplicate task. Nothing was changed.");
		task.done = object.value(QStringLiteral("done")).toBool();
		task.notes = object.value(QStringLiteral("notes")).toString();
		if (task.notes.size() > MaximumNotesLength)
			return QStringLiteral("A task description is too long. Nothing was changed.");
		const QJsonArray steps = object.value(QStringLiteral("steps")).toArray();
		if (steps.size() > MaximumSteps)
			return QStringLiteral("A task has too many steps. Nothing was changed.");
		for (const QJsonValue &stepValue : steps)
		{
			const QJsonObject step = stepValue.toObject();
			const QString text = step.value(QStringLiteral("text")).toString().simplified();
			if (!stepValue.isObject() || text.isEmpty() || text.size() > MaximumStepLength
				|| !step.value(QStringLiteral("done")).isBool())
				return QStringLiteral("A checklist step is invalid. Nothing was changed.");
			task.steps.append(Step{ text, step.value(QStringLiteral("done")).toBool() });
		}
		ids.insert(task.id);
		imported.append(task);
	}
	const QString active = root.value(QStringLiteral("activeTaskId")).toString();
	if (!active.isEmpty() && !ids.contains(active))
		return QStringLiteral("The active task is missing from the file. Nothing was changed.");
	for (const Task &task : imported)
		if (task.id == active && task.done)
			return QStringLiteral("A finished task cannot be active. Nothing was changed.");

	// Write the proposed state before publishing it, and restore memory if writing
	// fails. No signals or event processing occur while the candidate is installed.
	const QVector<Task> previous = _tasks;
	const QString previousActive = _activeId;
	_tasks = imported;
	_activeId = active;
	const QString failure = writeFile(storagePath());
	_tasks = previous;
	_activeId = previousActive;
	if (!failure.isEmpty())
		return failure;
	_saveTimer.stop();
	beginResetModel();
	_tasks = imported;
	_activeId = active;
	endResetModel();
	emit countChanged();
	emit activeChanged();
	return QString();
}

QString TaskList::writeFile(const QString &path) const
{

	if (!QDir().mkpath(QFileInfo(path).absolutePath()))
	{
		return QStringLiteral("Could not create the task file's directory.");
	}

	QJsonArray	array;

	for (const Task &task : _tasks)
	{
		QJsonObject	object;

		object.insert(QStringLiteral("id"), task.id);
		object.insert(QStringLiteral("title"), task.title);
		object.insert(QStringLiteral("done"), task.done);
		object.insert(QStringLiteral("estimate"), task.estimate);
		object.insert(QStringLiteral("completed"), task.completed);

		// Left out when empty, so a list without them reads as it always did.
		if (!task.notes.isEmpty())
			object.insert(QStringLiteral("notes"), task.notes);

		if (!task.steps.isEmpty())
		{
			QJsonArray	steps;

			for (const Step &step : task.steps)
				steps.append(QJsonObject{ { QStringLiteral("text"), step.text }, { QStringLiteral("done"), step.done } });

			object.insert(QStringLiteral("steps"), steps);
		}

		array.append(object);
	}

	QJsonObject	root;

	root.insert(QStringLiteral("version"), FormatVersion);
	root.insert(QStringLiteral("activeTaskId"), _activeId);
	root.insert(QStringLiteral("tasks"), array);

	QSaveFile	file(path);

	if (!file.open(QIODevice::WriteOnly))
	{
		return file.errorString();
	}

	const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
	if (file.write(bytes) != bytes.size())
		return file.errorString();

	if (!file.commit())
		return file.errorString();
	return QString();
}
