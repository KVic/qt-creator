/****************************************************************************
**
** Copyright (C) 2016 Lorenz Haas
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

// Tested with version 0.59 and 0.60

#include "uncrustify.h"

#include "uncrustifyconstants.h"
#include "uncrustifyoptionspage.h"
#include "uncrustifysettings.h"

#include "../beautifierconstants.h"
#include "../beautifierplugin.h"

#include <coreplugin/actionmanager/actioncontainer.h>
#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/actionmanager/command.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/idocument.h>
#include <cppeditor/cppeditorconstants.h>
#include <texteditor/texteditor.h>
#include <utils/fileutils.h>

#include <QAction>
#include <QMenu>

namespace Beautifier {
namespace Internal {
namespace Uncrustify {

Uncrustify::Uncrustify(BeautifierPlugin *parent) :
    BeautifierAbstractTool(parent),
    m_beautifierPlugin(parent),
    m_settings(new UncrustifySettings)
{
}

Uncrustify::~Uncrustify()
{
    delete m_settings;
}

bool Uncrustify::initialize()
{
    Core::ActionContainer *menu = Core::ActionManager::createMenu(Constants::Uncrustify::MENU_ID);
    menu->menu()->setTitle(tr("&Uncrustify"));

    m_formatFile = new QAction(BeautifierPlugin::msgFormatCurrentFile(), this);
    Core::Command *cmd
            = Core::ActionManager::registerAction(m_formatFile,
                                                  Constants::Uncrustify::ACTION_FORMATFILE);
    menu->addAction(cmd);
    connect(m_formatFile, &QAction::triggered, this, &Uncrustify::formatFile);

    m_formatRange = new QAction(BeautifierPlugin::msgFormatSelectedText(), this);
    cmd = Core::ActionManager::registerAction(m_formatRange,
                                              Constants::Uncrustify::ACTION_FORMATSELECTED);
    menu->addAction(cmd);
    connect(m_formatRange, &QAction::triggered, this, &Uncrustify::formatSelectedText);

    Core::ActionManager::actionContainer(Constants::MENU_ID)->addMenu(menu);

    connect(m_settings, &UncrustifySettings::supportedMimeTypesChanged,
            [this] { updateActions(Core::EditorManager::currentEditor()); });

    return true;
}

QString Uncrustify::id() const
{
    return QLatin1String(Constants::Uncrustify::DISPLAY_NAME);
}

void Uncrustify::updateActions(Core::IEditor *editor)
{
    const bool enabled = editor && m_settings->isApplicable(editor->document());
    m_formatFile->setEnabled(enabled);
    m_formatRange->setEnabled(enabled);
}

QList<QObject *> Uncrustify::autoReleaseObjects()
{
    return {new UncrustifyOptionsPage(m_settings, this)};
}

void Uncrustify::formatFile()
{
    m_beautifierPlugin->formatCurrentFile(command(false));
}

void Uncrustify::formatSelectedText()
{
    const TextEditor::TextEditorWidget *widget
            = TextEditor::TextEditorWidget::currentTextEditorWidget();
    if (!widget)
        return;

    QTextCursor tc = widget->textCursor();
    if (tc.hasSelection()) {
        // Extend selection to full lines
        const int posSelectionEnd = tc.selectionEnd();
        tc.setPosition(tc.selectionStart());
        tc.movePosition(QTextCursor::StartOfLine);
        const int startPos = tc.position();
        tc.setPosition(posSelectionEnd);
        // Don't extend the selection if the cursor is at the start of the line
        if (tc.positionInBlock() > 0)
            tc.movePosition(QTextCursor::EndOfLine);
        const int endPos = tc.position();
        m_beautifierPlugin->formatCurrentFile(command(true), startPos, endPos);
    } else if (m_settings->formatEntireFileFallback()) {
        formatFile();
    }
}

ConfigurationSpecification Uncrustify::configurationSpecification() const
{
    ConfigurationSpecification spec;

    spec.useCustomStyle = m_settings->useCustomStyle();
    spec.customStyleFilePath = Utils::FileName::fromString(
        m_settings->styleFileName(m_settings->customStyle()));
    spec.useProjectFile = m_settings->useOtherFiles();
    spec.projectFileMask = "*uncrustify.cfg";
    spec.useSpecificFile = m_settings->useSpecificConfigFile();
    spec.specificFilePath = m_settings->specificConfigFile();
    spec.useHomeFile = m_settings->useHomeFile();
    spec.homeFileNames.append("uncrustify.cfg");

    return spec;
}

Command Uncrustify::command() const
{
    return command(false);
}

bool Uncrustify::isApplicable(const Core::IDocument *document) const
{
    return m_settings->isApplicable(document);
}

Command Uncrustify::command(bool fragment) const
{
    Command command;
    command.setExecutable(m_settings->command());
    command.setSpecification(configurationSpecification());
    command.setProcessing(Command::PipeProcessing);
    if (m_settings->version() >= 62) {
        command.addOption("--assume");
        command.addOption("%file");
    } else {
        command.addOption("-l");
        command.addOption("cpp");
    }
    command.addOption("-L");
    command.addOption("1-2");
    if (fragment)
        command.addOption("--frag");
    command.addOption("-c");
    command.addOption("%configFile");
    return command;
}

} // namespace Uncrustify
} // namespace Internal
} // namespace Beautifier
