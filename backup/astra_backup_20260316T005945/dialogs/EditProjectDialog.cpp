#include "EditProjectDialog.h"
#include "models/Project.h"
#include <QPixmap>
#include <QLineEdit>
#include <QTextEdit>
#include <QLabel>

EditProjectDialog::EditProjectDialog(std::shared_ptr<Project> project, QWidget *parent)
    : NewProjectDialog(parent)
    , _project(project)
{
    setWindowTitle("Edit Project");
    
    if (_project) {
        populateFields();
    }
}

void EditProjectDialog::populateFields()
{
    // Populate the name field
    _nameEdit->setText(_project->getName());
    
    // Populate the description field
    _descriptionEdit->setPlainText(_project->getDescription());
    
    // Populate the thumbnail if it exists
    QString thumbPath = _project->getImagePath();
    if (!thumbPath.isEmpty()) {
        _thumbnailPath = thumbPath;
        QPixmap pixmap(thumbPath);
        if (!pixmap.isNull()) {
            _thumbnailPreview->setPixmap(pixmap.scaled(100, 100, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
    
    // The OK button should be enabled since we have a valid project name
    validateInput();
}

void EditProjectDialog::applyChanges()
{
    if (!_project) {
        return;
    }
    
    // Update project fields (setters will automatically update modified date)
    _project->setName(projectName());
    _project->setDescription(projectDescription());
    _project->setImagePath(thumbnailPath());
}