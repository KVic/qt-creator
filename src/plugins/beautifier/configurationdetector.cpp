/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "configurationdetector.h"

#include <projectexplorer/project.h>
#include <projectexplorer/projectnodes.h>
#include <projectexplorer/projecttree.h>

#include <QDir>
#include <QRegExp>

namespace Beautifier {
namespace Internal {

namespace {

using namespace ProjectExplorer;

const FileNode *findConfigFileNode(const QRegExp &configFileMask, const FolderNode *from,
                                   const FolderNode *visited)
{
    const QList<Node *> nodes = from->nodes();

    for (const Node *node:nodes)
        if (node == visited)
            continue;
        else if (const FileNode *file = node->asFileNode())
            if (configFileMask.exactMatch(file->filePath().fileName()))
                return file;

    for (const Node *node : nodes)
        if (node == visited)
            continue;
        else if (const FolderNode *folder = node->asFolderNode()) {
            if (const FileNode *file = findConfigFileNode(configFileMask, folder, visited))
                return file;
        }

    return nullptr;
}

const FileNode *findConfigFileNode(const QRegExp &configFileMask, const FolderNode *from)
{
    if (!from)
        return nullptr;

    const Project *project = ProjectTree::currentProject();
    if (!project)
        return nullptr;

    const Node *root = project->rootProjectNode();
    if (!root)
        return nullptr;

    const FolderNode *visited = nullptr;
    const FolderNode *current = from;
    const FileNode *config = findConfigFileNode(configFileMask, current, visited);

    while (!config && visited != root) {
        // TODO: Detect the product boundary.

        visited = current;
        current = current->parentFolderNode();
        config = findConfigFileNode(configFileMask, current, visited);
    }

    return config;
}

const Node *findSourceFileNode(const QString &sourceFilePath)
{
    const Node *node = ProjectTree::findCurrentNode();
    Q_ASSERT(node && node->filePath().toString() == sourceFilePath);
    return node;
}

QRegExp configFileRegExp(const QString &configFileMask)
{
    QRegExp re(configFileMask);
    re.setPatternSyntax(QRegExp::Wildcard);
    re.setCaseSensitivity(Qt::CaseInsensitive);
    return re;
}

QString findConfigFileForSource(const QString &configFileMask, const QString &sourceFilePath)
{
    const Node *sourceNode = findSourceFileNode(sourceFilePath);
    if (sourceNode) {
        const FileNode *configNode = findConfigFileNode(configFileRegExp(configFileMask),
                                                        sourceNode->parentFolderNode());
        if (configNode)
            return configNode->filePath().toString();
    }

    return QString();
}

QString findFirstConfigFile(const QString &configFileMask)
{
    if (const Project *project = ProjectTree::currentProject()) {
        const QStringList files = project->files(Project::AllFiles);
        QRegExp re = configFileRegExp(configFileMask);
        for (const QString &file : files) {
            if (re.exactMatch(QFileInfo(file).fileName()))
                return file;
        }
    }

    return QString();
}

QString verifyConfigFile(const QString &filePath)
{
    if (!filePath.isEmpty() && QFile::exists(filePath))
        return filePath;

    return QString();
}

QString verifyConfigFile(const Utils::FileName &filePath)
{
    if (filePath.exists())
        return filePath.toString();

    return QString();
}

} // namespace

QString ConfigurationDetector::detectConfiguration(const ConfigurationSpecification &specification,
                                                   const QString &sourceFilePath)
{
    if (specification.useCustomStyle) {
        const QString configFileName = detectCustomStyleFile(specification.customStyleFilePath);
        if (!configFileName.isEmpty())
            return configFileName;
    }

    if (specification.useProjectFile) {
        const QString configFileName = detectProjectFile(specification.projectFileMask,
                                                         sourceFilePath);
        if (!configFileName.isEmpty())
            return configFileName;
    }

    if (specification.useSpecificFile) {
        const QString configFileName = detectSpecificFile(specification.specificFilePath);
        if (!configFileName.isEmpty())
            return configFileName;
    }

    if (specification.useHomeFile) {
        const QString configFileName = detectHomeFile(specification.homeFileNames);
        if (!configFileName.isEmpty())
            return configFileName;
    }

    return QString();
}

QString ConfigurationDetector::detectCustomStyleFile(const Utils::FileName &configFilePath)
{
    return verifyConfigFile(configFilePath);
}

QString ConfigurationDetector::detectProjectFile(const QString &configFileMask,
                                                 const QString &sourceFilePath)
{
    if (!sourceFilePath.isEmpty())
        return verifyConfigFile(findConfigFileForSource(configFileMask, sourceFilePath));
    else
        return verifyConfigFile(findFirstConfigFile(configFileMask));
}

QString ConfigurationDetector::detectSpecificFile(const Utils::FileName &configFilePath)
{
    return verifyConfigFile(configFilePath);
}

QString ConfigurationDetector::detectHomeFile(const QStringList &configFileNames)
{
    const QDir home = QDir::home();
    for (const QString &fileName : configFileNames) {
        const QString filePath = home.filePath(fileName);
        if (!verifyConfigFile(filePath).isEmpty())
            return filePath;
    }

    return QString();
}

} // namespace Internal
} // namespace Beautifier
