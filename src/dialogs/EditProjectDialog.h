#ifndef EDITPROJECTDIALOG_H
#define EDITPROJECTDIALOG_H

#include "NewProjectDialog.h"
#include <memory>

class Project;

class EditProjectDialog : public NewProjectDialog
{
    Q_OBJECT

public:
    explicit EditProjectDialog(std::shared_ptr<Project> project, QWidget *parent = nullptr);
    
    // Apply the dialog changes to the project object
    void applyChanges();

private:
    void populateFields();
    
    std::shared_ptr<Project> _project;
};

#endif // EDITPROJECTDIALOG_H