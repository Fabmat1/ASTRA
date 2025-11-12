#ifndef NEWPROJECTDIALOG_H
#define NEWPROJECTDIALOG_H

#include <QDialog>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QTextEdit;
class QLabel;
QT_END_NAMESPACE

class NewProjectDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NewProjectDialog(QWidget *parent = nullptr);

    QString projectName() const;
    QString projectDescription() const;
    QString thumbnailPath() const;

protected slots:
    void onBrowseThumbnail();
    void validateInput();

protected:
    QLineEdit* _nameEdit;
    QTextEdit* _descriptionEdit;
    QLabel* _thumbnailPreview;
    QString _thumbnailPath;
    QPushButton* _okButton;
};

#endif // NEWPROJECTDIALOG_H