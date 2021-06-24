#include "ModelManager.h"
#include "Network.h"
#include <QSettings>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <iostream>
// libarchive
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>

namespace {
    /**
     * Give it a QStringList with multiple paths, and this will return the
     * path prefix (i.e. the path to the shared root directory). Note: if only
     * a single path is given, the prefix is the dirname part of the path.
     */
    QString getCommonPrefixPath(QStringList list) {
        auto it = list.begin();

        if (it == list.end())
            return QString();

        QString prefix = *it++;

        for (; it != list.end(); ++it) {
            auto offsets = std::mismatch(
                prefix.begin(), prefix.end(),
                it->begin(), it->end());

            // If we found a mismatch, we found a substring in it that's a
            // shorter common prefix.
            if (offsets.first != prefix.end())
                prefix = it->sliced(0, offsets.second - it->begin());
        }

        // prefix.section("/", 0, -1)?
        return prefix.section("/", 0, -2);
    }
}


ModelManager::ModelManager(QObject *parent)
    : QAbstractTableModel(parent)
    , network_(new Network(this))
    , isFetchingRemoteModels_(false)
{
    // Create/Load Settings and create a directory on the first run. Use mock QSEttings, because we want nativeFormat, but we don't want ini on linux.
    // NativeFormat is not always stored in config dir, whereas ini is always stored. We used the ini format to just get a path to a dir.
    configDir_ = QFileInfo(QSettings(QSettings::IniFormat, QSettings::UserScope, "translateLocally", "translateLocally").fileName()).absoluteDir();
    if (!QDir(configDir_).exists()) {
        if (QFileInfo::exists(configDir_.absolutePath())) {
            std::cerr << "We want to store data at a directory at: " << configDir_.absolutePath().toStdString() << " but a file with the same name exists." << std::endl;
        } else {
            QDir().mkpath(configDir_.absolutePath());
        }
    }
    startupLoad();
}

bool ModelManager::isManagedModel(Model const &model) const {
    return model.isLocal() && model.path.startsWith(configDir_.absolutePath());
}

bool ModelManager::validateModel(QString path) {
    QJsonObject obj = getModelInfoJsonFromDir(path);
    if (obj.find("path") == obj.end()) {
        emit error(tr("Failed to find, open or parse the model_info.json in %1").arg(path));
        return false;
    }

    if (!parseModelInfo(obj).isLocal()) // parseModelInfo emits its own error signals
        return false;

    return true;
}

Model ModelManager::writeModel(QFile *file, QString filename) {
    // Default value for filename is the basename of the file.
    if (filename.isEmpty())
        filename = QFileInfo(*file).fileName();
    
    // Initially extract to to a temporary directory. Will delete its contents
    // when it goes out of scope
    // QTemporaryDir tempDir(configDir_.filePath(".extracting-XXXXXXX"));
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        emit error(tr("Could not create temporary directory in {} to extract the model archive to.").arg(configDir_.absolutePath()));
        return Model{};
    }

    // Try to extract the archive to the temporary directory
    QStringList extracted;
    if (!extractTarGz(file, tempDir.path(), extracted))
        return Model{};

    qDebug() << "Extracted: " << extracted;

    // Assert we extracted at least something.
    if (extracted.isEmpty()) {
        emit error(tr("Did not extract any files from the model archive."));
        return Model{};
    }

    // Get the common prefix of all files. In the ideal case, it's the same as
    // tempDir, but the archive might have had it's own sub folder.
    QString prefix = getCommonPrefixPath(extracted);
    qDebug() << "Common prefix: " << prefix;

    if (prefix.isEmpty()) {
        emit error(tr("Could not determine prefix path of extracted model."));
        return Model{};
    }

    // Try determining whether the model is any good before we continue to safe
    // it to a permanent destination
    if (!validateModel(prefix)) // validateModel emits its own error() signals (hence validateModel and not isModelValid)
        return Model{};

    QString newModelDirName = QString("%1-%2").arg(filename.split(".tar.gz")[0]).arg(QDateTime::currentSecsSinceEpoch());
    QString newModelDirPath = configDir_.absoluteFilePath(newModelDirName);

    qDebug() << "Rename " << prefix << " to " << newModelDirPath;

    // Q_ASSERT(prefix exists in tempDir)
    if (!QDir().rename(prefix, newModelDirPath)) {
        emit error(tr("Could not move extracted model from %1 to %2.").arg(tempDir.path()).arg(newModelDirPath));
        return Model{};
    }

    // Don't try to delete the no-longer-temporary directry
    tempDir.setAutoRemove(false);

    QJsonObject obj = getModelInfoJsonFromDir(newModelDirPath);
    Q_ASSERT(obj.find("path") != obj.end());

    Model model = parseModelInfo(obj);
    insertLocalModel(model);
    updateAvailableModels();
    
    return model;
}

bool ModelManager::removeModel(Model const &model) {
    if (!isManagedModel(model))
        return false;

    QDir modelDir = QDir(model.path);

    // First attempt to remove the model_info.json file as a test. If that works
    // we know that at least the model won't be loaded on next scan/start-up.

    if (!modelDir.remove("model_info.json")) {
        emit error(tr("Could not delete %1/model_info.json").arg(model.path));
        return false;
    }

    if (!modelDir.removeRecursively()) {
        emit error(tr("Could not completely remove the model directory %1").arg(model.path));
        // no return here because we did remove model_info.json already, so we
        // should also remove the model from localModels_
    }

    int position = localModels_.indexOf(model);

    if (position == -1)
        return false;

    beginRemoveRows(QModelIndex(), position, position + 1);
    localModels_.removeOne(model);
    endRemoveRows();
    updateAvailableModels();
    return true;
}

bool ModelManager::insertLocalModel(Model model) {
    int position = 0;

    for (int i = 0; i < localModels_.size(); ++i) {
        // First, make sure we don't already have this model
        if (localModels_[i].isSameModel(model)) {
            localModels_[i] = model;
            emit dataChanged(index(i, 0), index(i, columnCount()));
            return false;
        }

        // Second, while we're iterating anyway, figure out where to insert
        // this model.
        if (localModels_[i] < model)
            position = i + 1;
    }

    beginInsertRows(QModelIndex(), position, position + 1);
    localModels_.append(model);
    endInsertRows();
    return true;
}

QJsonObject ModelManager::getModelInfoJsonFromDir(QString dir) {
    // Check if we can find a model_info.json in the directory. If so, record it as part of the model
    QFileInfo modelInfo(dir + "/model_info.json");
    if (modelInfo.exists()) {
        QFile modelInfoFile(modelInfo.absoluteFilePath());
        bool isOpen = modelInfoFile.open(QIODevice::ReadOnly | QIODevice::Text);
        if (isOpen) {
            QByteArray bytes = modelInfoFile.readAll();
            modelInfoFile.close();
            // Parse the Json
            QJsonDocument jsonResponse = QJsonDocument::fromJson(bytes);
            QJsonObject obj = jsonResponse.object();
            // Populate the json with path
            obj.insert(QString("path"), QJsonValue(dir));
            return obj;
        } else {
            emit error(tr("Failed to open json config file: %1").arg(modelInfo.absoluteFilePath()));
            return QJsonObject();
        }
    } else {
        // Model info doesn't exist or a configuration file is not found. Handle the error elsewhere.
        return QJsonObject();
    }
}

Model ModelManager::parseModelInfo(QJsonObject& obj, translateLocally::models::Location type) {
    using namespace translateLocally::models;
    std::vector<QString> keysSTR = {QString{"shortName"},
                                    QString{"modelName"},
                                    QString{"src"},
                                    QString{"trg"},
                                    QString{"type"}};
    std::vector<QString> keysFLT{QString("version"), QString("API")};
    QString criticalKey = type==Local ? QString("path") : QString("url");

    Model model = {};
    // Non critical keys. Some of them might be missing from old model versions but we don't care
    for (auto&& key : keysSTR) {
        auto iter = obj.find(key);
        if (iter != obj.end()) {
            model.set(key, iter.value().toString());
        } else {
            model.set(key, "");
        }
    }

    // Float Keys depend on whether we have a local or a remote model
    // Non critical if missing due to older file name
    for (auto&& key : keysFLT) {
        QString keyname = type==Local ? "local" + key : "remote" + key;
        auto iter = obj.find(key);
        if (iter != obj.end()) {
            model.set(keyname, (float)iter.value().toDouble());
        } else {
            model.set(keyname, "");
        }
    }

    // Critical key. If this key is missing the json is completely invalid and needs to be discarded
    // it's either the path to the model or the url to its download location
    auto iter = obj.find(criticalKey);
    if (iter != obj.end()) {
        model.set(criticalKey, iter.value().toString());
    } else {
        emit error(tr("The json file provided is missing '%1' or is corrupted. Please redownload the model. "
                      "If the path variable is missing, it is added automatically, so please file a bug report at: https://github.com/XapaJIaMnu/translateLocally/issues").arg(criticalKey));
        return Model{};
    }

    return model;
}

void ModelManager::scanForModels(QString path) {
    //Iterate over all files in the folder and take note of available models and archives
    //@TODO currently, archives can only be extracted from the config dir
    QDirIterator it(path, QDir::NoFilter);
    QList<Model> models;
    while (it.hasNext()) {
        QString current = it.next();
        QFileInfo f(current);
        if (f.isDir()) {
            QJsonObject obj = getModelInfoJsonFromDir(current);
            if (!obj.empty()) {
                Model model = parseModelInfo(obj);
                if (model.path != "") {
                    insertLocalModel(model);
                } else {
                    emit error(tr("Corrupted json file: %1/model_info.json. Delete or redownload.").arg(current));
                }
            } else {
                // We have a folder in our models directory that doesn't contain a model. This is ok.
                continue;
            }
        } else {
            // Check if this an existing archive
            if (f.completeSuffix() == QString("tar.gz")) {
                archives_.append(f.fileName());
            }
        }
    }

    updateAvailableModels();
}

void ModelManager::startupLoad() {
    //Iterate over all files in the config folder and take note of available models and archives
    scanForModels(configDir_.absolutePath());
    scanForModels(QDir::current().path()); // Scan the current directory for models. @TODO archives found in this folder would not be used
}

// Adapted from https://github.com/libarchive/libarchive/blob/master/examples/untar.c#L136
bool ModelManager::extractTarGz(QFile *file, QDir const &destination, QStringList &files) {
    // Change current working directory while extracting
    QString currentPath = QDir::currentPath();

    if (!QDir::setCurrent(destination.absolutePath())) {
        emit error(tr("Failed to change path to the configuration directory %1. %2 won't be extracted.").arg(destination.absolutePath()).arg(file->fileName()));
        return false;
    }

    QStringList extracted;
    bool success = extractTarGzInCurrentPath(file, extracted);

    for (QString const &file : extracted)
        files << destination.filePath(file);

    QDir::setCurrent(currentPath);
    return success;
}

bool ModelManager::extractTarGzInCurrentPath(QFile *file, QStringList &files) {
    auto warn = [&](const char *f, const char *m) {
        emit error(tr("Trouble while extracting language model after call to %1: %2").arg(f).arg(m));
    };

    auto copy_data = [=](struct archive *a_in, struct archive *a_out) {
        const void *buff;
        size_t size;
#if ARCHIVE_VERSION_NUMBER >= 3000000
        int64_t offset;
#else
        off_t offset;
#endif

        for (;;) {
            int retval = archive_read_data_block(a_in, &buff, &size, &offset);
            // End of archive: good!
            if (retval == ARCHIVE_EOF)
                return ARCHIVE_OK;
            
            // Not end of archive: bad.
            if (retval != ARCHIVE_OK) {
                warn("archive_read_data_block()", archive_error_string(a_in));
                return retval;
            }
            
            retval = archive_write_data_block(a_out, buff, size, offset);
            if (retval != ARCHIVE_OK) {
                warn("archive_write_data_block()", archive_error_string(a_out));
                return retval;
            }
        }
    };

    archive *a_in = archive_read_new();
    archive *a_out = archive_write_disk_new();
    archive_write_disk_set_options(a_out, ARCHIVE_EXTRACT_TIME);
    
    archive_read_support_format_tar(a_in);
    archive_read_support_filter_gzip(a_in);
    
    if (!file->open(QIODevice::ReadOnly)) {
        emit error(tr("Trouble while extracting language model after call to %1: %2").arg("QIODevice::open()").arg(file->errorString()));
        return false;
    }
    
    if (archive_read_open_fd(a_in, file->handle(), 10240)) {
        warn("archive_read_open_filename()", archive_error_string(a_in));
        return false;
    }

    // Read (and extract) all archive entries
    for (;;) {
        archive_entry *entry;
        
        int retval = archive_read_next_header(a_in, &entry);

        // Stop when we read past the last entry
        if (retval == ARCHIVE_EOF)
            break;

        if (retval < ARCHIVE_OK)
            warn("archive_read_next_header()", archive_error_string(a_in));
        if (retval < ARCHIVE_WARN)
            return false;

        retval = archive_write_header(a_out, entry);
        if (retval < ARCHIVE_OK)
            warn("archive_write_header()", archive_error_string(a_out));
        else {
            files << QString(archive_entry_pathname(entry));

            if(archive_entry_size(entry) > 0)
                if (copy_data(a_in, a_out) < ARCHIVE_WARN)
                    return false;
        }

        retval = archive_write_finish_entry(a_out);
        if (retval < ARCHIVE_OK)
            warn("archive_write_finish_entry()", archive_error_string(a_out));
        if (retval < ARCHIVE_WARN)
            return false;
    }

    archive_read_close(a_in);
    archive_read_free(a_in);

    archive_write_close(a_out);
    archive_write_free(a_out);

    return true;
}

void ModelManager::fetchRemoteModels() {
    if (isFetchingRemoteModels())
        return;

    isFetchingRemoteModels_ = true;
    emit fetchingRemoteModels();

    QUrl url("http://data.statmt.org/bergamot/models/models.json");
    QNetworkRequest request(url);
    QNetworkReply *reply = network_->get(request);
    connect(reply, &QNetworkReply::finished, this, [=] {
        switch (reply->error()) {
            case QNetworkReply::NoError:
                parseRemoteModels(QJsonDocument::fromJson(reply->readAll()).object());
                break;
            default:
                emit error(reply->errorString());
                break;
        }

        isFetchingRemoteModels_ = false;
        emit fetchedRemoteModels();

        reply->deleteLater();
    });
}

void ModelManager::parseRemoteModels(QJsonObject obj) {
    using namespace translateLocally::models;
    remoteModels_.clear();
    
    for (auto&& arrobj : obj["models"].toArray()) {
        QJsonObject obj = arrobj.toObject();
        remoteModels_.append(parseModelInfo(obj, Remote));
    }

    std::sort(remoteModels_.begin(), remoteModels_.end());
    updateAvailableModels();
}

QList<Model> ModelManager::getInstalledModels() const {
    return localModels_;
}

QList<Model> ModelManager::getRemoteModels() const {
    return remoteModels_;
}

QList<Model> ModelManager::getNewModels() const {
    return newModels_;
}

QList<Model> ModelManager::getUpdatedModels() const {
    return updatedModels_;
}

void ModelManager::updateAvailableModels() {
    newModels_.clear();
    updatedModels_.clear();

    for (auto &&model : remoteModels_) {
        bool installed = false;
        bool outdated = false;
        for (int i = 0; i < localModels_.size(); ++i) {
            if (localModels_[i].isSameModel(model)) {
                localModels_[i].remoteAPI = model.remoteAPI;
                localModels_[i].remoteversion = model.remoteversion;
                installed = true;
                outdated = localModels_[i].outdated();
                emit dataChanged(index(i, 0), index(i, columnCount()));
                break;
            }
        }

        if (!installed) {
            newModels_.append(model);
        }
        if (outdated) {
            updatedModels_.append(model);
        }
    }

    emit localModelsChanged();
}

int ModelManager::rowCount(const QModelIndex &parent) const {
    Q_UNUSED(parent);

    return localModels_.size();
} 

int ModelManager::columnCount(const QModelIndex &parent) const {
    Q_UNUSED(parent);

    return 2;
}

QVariant ModelManager::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole)
        return QVariant();

    if (orientation != Qt::Horizontal)
        return QVariant();

    switch (section) {
        case Column::Name:
            return tr("Name", "translation model name");
        case Column::Version:
            return tr("Version", "translation model version");
        default:
            return QVariant();
    }
}

QVariant ModelManager::data(const QModelIndex &index, int role) const {
    if (index.row() >= localModels_.size())
        return QVariant();

    Model model = localModels_[index.row()];

    if (role == Qt::UserRole)
        return QVariant::fromValue(model);

    switch (index.column()) {
        case Column::Name:
            switch (role) {
                case Qt::DisplayRole:
                    return model.modelName;
                default:
                    return QVariant();
            }

        case Column::Version:
            switch (role) {
                case Qt::DisplayRole:
                    return model.localversion;
                case Qt::TextAlignmentRole:
                    // @TODO figure out how to compile combined flag as below. 
                    // Error is "can't convert the result to QVariant."
                    // return Qt::AlignRight | Qt::AlignBaseline;
                    return Qt::AlignRight;
                default:
                    return QVariant();
            }
    }

    return QVariant();
}
