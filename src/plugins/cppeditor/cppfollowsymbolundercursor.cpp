/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/


#include "cppfollowsymbolundercursor.h"
#include "cppeditor.h"

#include <cplusplus/ASTPath.h>
#include <cplusplus/BackwardsScanner.h>
#include <cplusplus/ExpressionUnderCursor.h>
#include <cplusplus/SimpleLexer.h>
#include <cplusplus/TypeOfExpression.h>
#include <cpptools/cppmodelmanagerinterface.h>
#include <cpptools/symbolfinder.h>
#include <texteditor/basetextdocumentlayout.h>
#include <utils/qtcassert.h>

#include <QList>
#include <QSet>

using namespace CPlusPlus;
using namespace CppTools;
using namespace CppEditor;
using namespace CppEditor::Internal;
using namespace TextEditor;

typedef BaseTextEditorWidget::Link Link;

namespace {

Link findMacroLink_helper(const QByteArray &name, Document::Ptr doc, const Snapshot &snapshot,
                          QSet<QString> *processed)
{
    if (doc && !name.startsWith('<') && !processed->contains(doc->fileName())) {
        processed->insert(doc->fileName());

        foreach (const Macro &macro, doc->definedMacros()) {
            if (macro.name() == name) {
                Link link;
                link.targetFileName = macro.fileName();
                link.targetLine = macro.line();
                return link;
            }
        }

        const QList<Document::Include> includes = doc->resolvedIncludes();
        for (int index = includes.size() - 1; index != -1; --index) {
            const Document::Include &i = includes.at(index);
            Link link = findMacroLink_helper(name, snapshot.document(i.resolvedFileName()),
                                             snapshot, processed);
            if (link.hasValidTarget())
                return link;
        }
    }

    return Link();
}

Link findMacroLink(const QByteArray &name, const Document::Ptr &doc)
{
    if (!name.isEmpty()) {
        if (doc) {
            const Snapshot snapshot = CppModelManagerInterface::instance()->snapshot();
            QSet<QString> processed;
            return findMacroLink_helper(name, doc, snapshot, &processed);
        }
    }

    return Link();
}

inline LookupItem skipForwardDeclarations(const QList<LookupItem> &resolvedSymbols)
{
    QList<LookupItem> candidates = resolvedSymbols;

    LookupItem result = candidates.first();
    const FullySpecifiedType ty = result.type().simplified();

    if (ty->isForwardClassDeclarationType()) {
        while (!candidates.isEmpty()) {
            LookupItem r = candidates.takeFirst();

            if (!r.type()->isForwardClassDeclarationType()) {
                result = r;
                break;
            }
        }
    }

    if (ty->isObjCForwardClassDeclarationType()) {
        while (!candidates.isEmpty()) {
            LookupItem r = candidates.takeFirst();

            if (!r.type()->isObjCForwardClassDeclarationType()) {
                result = r;
                break;
            }
        }
    }

    if (ty->isObjCForwardProtocolDeclarationType()) {
        while (!candidates.isEmpty()) {
            LookupItem r = candidates.takeFirst();

            if (!r.type()->isObjCForwardProtocolDeclarationType()) {
                result = r;
                break;
            }
        }
    }

    return result;
}

} // anonymous namespace

FollowSymbolUnderCursor::FollowSymbolUnderCursor(CPPEditorWidget *widget, const QTextCursor &cursor,
    bool resolveTarget, const Snapshot &snapshot, const Document::Ptr &documentFromSemanticInfo,
    CppTools::SymbolFinder *symbolFinder)
    : m_widget(widget)
    , m_cursor(cursor)
    , m_resolveTarget(resolveTarget)
    , m_snapshot(snapshot)
    , m_document(documentFromSemanticInfo)
    , m_symbolFinder(symbolFinder)
{
}

BaseTextEditorWidget::Link FollowSymbolUnderCursor::findLink()
{
    Link link;

    // Move to end of identifier
    QTextCursor tc = m_cursor;
    QChar ch = m_widget->document()->characterAt(tc.position());
    while (ch.isLetterOrNumber() || ch == QLatin1Char('_')) {
        tc.movePosition(QTextCursor::NextCharacter);
        ch = m_widget->document()->characterAt(tc.position());
    }

    // Try to match decl/def. For this we need the semantic doc with the AST.
    if (m_document
            && m_document->translationUnit()
            && m_document->translationUnit()->ast()) {
        int pos = tc.position();
        while (m_widget->document()->characterAt(pos).isSpace())
            ++pos;
        if (m_widget->document()->characterAt(pos) == QLatin1Char('(')) {
            link = attemptFuncDeclDef(m_cursor);
            if (link.hasValidLinkText())
                return link;
        }
    }

    int lineNumber = 0, positionInBlock = 0;
    m_widget->convertPosition(m_cursor.position(), &lineNumber, &positionInBlock);
    const unsigned line = lineNumber;
    const unsigned column = positionInBlock + 1;

    // Try to find a signal or slot inside SIGNAL() or SLOT()
    int beginOfToken = 0;
    int endOfToken = 0;

    SimpleLexer tokenize;
    tokenize.setQtMocRunEnabled(true);
    const QString blockText = m_cursor.block().text();
    const QList<Token> tokens = tokenize(blockText,
                                         BackwardsScanner::previousBlockState(m_cursor.block()));

    bool recognizedQtMethod = false;

    for (int i = 0; i < tokens.size(); ++i) {
        const Token &tk = tokens.at(i);

        if (((unsigned) positionInBlock) >= tk.begin()
                && ((unsigned) positionInBlock) <= tk.end()) {
            if (i >= 2 && tokens.at(i).is(T_IDENTIFIER) && tokens.at(i - 1).is(T_LPAREN)
                && (tokens.at(i - 2).is(T_SIGNAL) || tokens.at(i - 2).is(T_SLOT))) {

                // token[i] == T_IDENTIFIER
                // token[i + 1] == T_LPAREN
                // token[.....] == ....
                // token[i + n] == T_RPAREN

                if (i + 1 < tokens.size() && tokens.at(i + 1).is(T_LPAREN)) {
                    // skip matched parenthesis
                    int j = i - 1;
                    int depth = 0;

                    for (; j < tokens.size(); ++j) {
                        if (tokens.at(j).is(T_LPAREN)) {
                            ++depth;
                        } else if (tokens.at(j).is(T_RPAREN)) {
                            if (!--depth)
                                break;
                        }
                    }

                    if (j < tokens.size()) {
                        QTextBlock block = m_cursor.block();

                        beginOfToken = block.position() + tokens.at(i).begin();
                        endOfToken = block.position() + tokens.at(i).end();

                        tc.setPosition(block.position() + tokens.at(j).end());
                        recognizedQtMethod = true;
                    }
                }
            }
            break;
        }
    }

    // Now we prefer the doc from the snapshot with macros expanded.
    Document::Ptr doc = m_snapshot.document(m_widget->editorDocument()->filePath());
    if (!doc) {
        doc = m_document;
        if (!doc)
            return link;
    }

    if (!recognizedQtMethod) {
        const QTextBlock block = tc.block();
        int pos = m_cursor.positionInBlock();
        QChar ch = m_widget->document()->characterAt(m_cursor.position());
        if (pos > 0 && !(ch.isLetterOrNumber() || ch == QLatin1Char('_')))
            --pos; // positionInBlock points to a delimiter character.
        const Token tk = SimpleLexer::tokenAt(block.text(), pos,
                                              BackwardsScanner::previousBlockState(block), true);

        beginOfToken = block.position() + tk.begin();
        endOfToken = block.position() + tk.end();

        // Handle include directives
        if (tk.is(T_STRING_LITERAL) || tk.is(T_ANGLE_STRING_LITERAL)) {
            const unsigned lineno = m_cursor.blockNumber() + 1;
            foreach (const Document::Include &incl, doc->resolvedIncludes()) {
                if (incl.line() == lineno) {
                    link.targetFileName = incl.resolvedFileName();
                    link.linkTextStart = beginOfToken + 1;
                    link.linkTextEnd = endOfToken - 1;
                    return link;
                }
            }
        }

        if (tk.isNot(T_IDENTIFIER) && tk.kind() < T_FIRST_QT_KEYWORD && tk.kind() > T_LAST_KEYWORD)
            return link;

        tc.setPosition(endOfToken);
    }

    // Handle macro uses
    const Macro *macro = doc->findMacroDefinitionAt(line);
    if (macro) {
        QTextCursor macroCursor = m_cursor;
        const QByteArray name = CPPEditorWidget::identifierUnderCursor(&macroCursor).toLatin1();
        if (macro->name() == name)
            return link;    //already on definition!
    } else {
        const Document::MacroUse *use = doc->findMacroUseAt(endOfToken - 1);
        if (use && use->macro().fileName() != CppModelManagerInterface::configurationFileName()) {
            const Macro &macro = use->macro();
            link.targetFileName = macro.fileName();
            link.targetLine = macro.line();
            link.linkTextStart = use->begin();
            link.linkTextEnd = use->end();
            return link;
        }
    }

    // Find the last symbol up to the cursor position
    Scope *scope = doc->scopeAt(line, column);
    if (!scope)
        return link;

    // Evaluate the type of the expression under the cursor
    ExpressionUnderCursor expressionUnderCursor;
    QString expression = expressionUnderCursor(tc);

    for (int pos = tc.position();; ++pos) {
        const QChar ch = m_widget->document()->characterAt(pos);
        if (ch.isSpace())
            continue;
        if (ch == QLatin1Char('(') && !expression.isEmpty()) {
            tc.setPosition(pos);
            if (TextEditor::TextBlockUserData::findNextClosingParenthesis(&tc, true))
                expression.append(tc.selectedText());
        }

        break;
    }

    TypeOfExpression typeOfExpression;
    typeOfExpression.init(doc, m_snapshot);
    // make possible to instantiate templates
    typeOfExpression.setExpandTemplates(true);
    const QList<LookupItem> resolvedSymbols =
            typeOfExpression.reference(expression.toUtf8(), scope, TypeOfExpression::Preprocess);

    if (!resolvedSymbols.isEmpty()) {
        LookupItem result = skipForwardDeclarations(resolvedSymbols);

        foreach (const LookupItem &r, resolvedSymbols) {
            if (Symbol *d = r.declaration()) {
                if (d->isDeclaration() || d->isFunction()) {
                    const QString fileName = QString::fromUtf8(d->fileName(), d->fileNameLength());
                    if (m_widget->editorDocument()->filePath() == fileName) {
                        if (unsigned(lineNumber) == d->line()
                            && unsigned(positionInBlock) >= d->column()) { // TODO: check the end
                            result = r; // take the symbol under cursor.
                            break;
                        }
                    }
                } else if (d->isUsingDeclaration()) {
                    int tokenBeginLineNumber = 0, tokenBeginColumnNumber = 0;
                    m_widget->convertPosition(beginOfToken, &tokenBeginLineNumber,
                                              &tokenBeginColumnNumber);
                    if (unsigned(tokenBeginLineNumber) > d->line()
                            || (unsigned(tokenBeginLineNumber) == d->line()
                                && unsigned(tokenBeginColumnNumber) > d->column())) {
                        result = r; // take the symbol under cursor.
                        break;
                    }
                }
            }
        }

        if (Symbol *symbol = result.declaration()) {
            Symbol *def = 0;

            if (m_resolveTarget) {
                Symbol *lastVisibleSymbol = doc->lastVisibleSymbolAt(line, column);

                def = findDefinition(symbol, m_snapshot);

                if (def == lastVisibleSymbol)
                    def = 0; // jump to declaration then.

                if (symbol->isForwardClassDeclaration())
                    def = m_symbolFinder->findMatchingClassDeclaration(symbol, m_snapshot);
            }

            link = m_widget->linkToSymbol(def ? def : symbol);
            link.linkTextStart = beginOfToken;
            link.linkTextEnd = endOfToken;
            return link;
        }
    }

    // Handle macro uses
    QTextCursor macroCursor = m_cursor;
    const QByteArray name = CPPEditorWidget::identifierUnderCursor(&macroCursor).toLatin1();
    link = findMacroLink(name, m_document);
    if (link.hasValidTarget()) {
        link.linkTextStart = macroCursor.selectionStart();
        link.linkTextEnd = macroCursor.selectionEnd();
        return link;
    }

    return Link();
}

CPPEditorWidget::Link FollowSymbolUnderCursor::attemptFuncDeclDef(const QTextCursor &cursor)
{
    m_snapshot.insert(m_document);

    Link result;

    QList<AST *> path = ASTPath(m_document)(cursor);

    if (path.size() < 5)
        return result;

    NameAST *name = path.last()->asName();
    if (!name)
        return result;

    if (QualifiedNameAST *qName = path.at(path.size() - 2)->asQualifiedName()) {
        // TODO: check which part of the qualified name we're on
        if (qName->unqualified_name != name)
            return result;
    }

    for (int i = path.size() - 1; i != -1; --i) {
        AST *node = path.at(i);

        if (node->asParameterDeclaration() != 0)
            return result;
    }

    AST *declParent = 0;
    DeclaratorAST *decl = 0;
    for (int i = path.size() - 2; i > 0; --i) {
        if ((decl = path.at(i)->asDeclarator()) != 0) {
            declParent = path.at(i - 1);
            break;
        }
    }
    if (!decl || !declParent)
        return result;
    if (!decl->postfix_declarator_list || !decl->postfix_declarator_list->value)
        return result;
    FunctionDeclaratorAST *funcDecl = decl->postfix_declarator_list->value->asFunctionDeclarator();
    if (!funcDecl)
        return result;

    Symbol *target = 0;
    if (FunctionDefinitionAST *funDef = declParent->asFunctionDefinition()) {
        QList<Declaration *> candidates =
                m_symbolFinder->findMatchingDeclaration(LookupContext(m_document, m_snapshot),
                                                        funDef->symbol);
        if (!candidates.isEmpty()) // TODO: improve disambiguation
            target = candidates.first();
    } else if (declParent->asSimpleDeclaration()) {
        target = m_symbolFinder->findMatchingDefinition(funcDecl->symbol, m_snapshot);
    }

    if (target) {
        result = m_widget->linkToSymbol(target);

        unsigned startLine, startColumn, endLine, endColumn;
        m_document->translationUnit()->getTokenStartPosition(name->firstToken(), &startLine,
                                                             &startColumn);
        m_document->translationUnit()->getTokenEndPosition(name->lastToken() - 1, &endLine,
                                                           &endColumn);

        QTextDocument *textDocument = cursor.document();
        result.linkTextStart =
                textDocument->findBlockByNumber(startLine - 1).position() + startColumn - 1;
        result.linkTextEnd =
                textDocument->findBlockByNumber(endLine - 1).position() + endColumn - 1;
    }

    return result;
}

Symbol *FollowSymbolUnderCursor::findDefinition(Symbol *symbol, const Snapshot &snapshot) const
{
    if (symbol->isFunction())
        return 0; // symbol is a function definition.

    else if (!symbol->type()->isFunctionType())
        return 0; // not a function declaration

    return m_symbolFinder->findMatchingDefinition(symbol, snapshot);
}
