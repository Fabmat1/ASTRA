#include "NewProjectDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QPixmap>
#include <QGroupBox>

NewProjectDialog::NewProjectDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("New Project");
    setFixedSize(500, 400);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // Project name
    QGroupBox* nameGroup = new QGroupBox("Project Name *", this);
    QVBoxLayout* nameLayout = new QVBoxLayout(nameGroup);
    _nameEdit = new QLineEdit(this);
    _nameEdit->setPlaceholderText("Enter project name...");
    nameLayout->addWidget(_nameEdit);
    mainLayout->addWidget(nameGroup);

    // Project description
    QGroupBox* descGroup = new QGroupBox("Description (Optional)", this);
    QVBoxLayout* descLayout = new QVBoxLayout(descGroup);
    _descriptionEdit = new QTextEdit(this);
    _descriptionEdit->setPlaceholderText("Enter project description...");
    _descriptionEdit->setMaximumHeight(100);
    descLayout->addWidget(_descriptionEdit);
    mainLayout->addWidget(descGroup);

    // Thumbnail
    QGroupBox* thumbGroup = new QGroupBox("Thumbnail (Optional)", this);
    QVBoxLayout* thumbLayout = new QVBoxLayout(thumbGroup);
    
    QHBoxLayout* thumbHLayout = new QHBoxLayout();
    _thumbnailPreview = new QLabel(this);
    _thumbnailPreview->setFixedSize(100, 100);
    _thumbnailPreview->setStyleSheet("border: 1px solid #ccc; background-color: #f0f0f0;");
    _thumbnailPreview->setAlignment(Qt::AlignCenter);
    _thumbnailPreview->setText("No image");
    
    QPushButton* browseButton = new QPushButton("Browse...", this);
    connect(browseButton, &QPushButton::clicked, this, &NewProjectDialog::onBrowseThumbnail);
    
    thumbHLayout->addWidget(_thumbnailPreview);
    thumbHLayout->addWidget(browseButton);
    thumbHLayout->addStretch();
    thumbLayout->addLayout(thumbHLayout);
    mainLayout->addWidget(thumbGroup);

    mainLayout->addStretch();

    // Buttons
    QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    _okButton = buttonBox->button(QDialogButtonBox::Ok);
    _okButton->setEnabled(false);
    
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);

    // Validate input
    connect(_nameEdit, &QLineEdit::textChanged, this, &NewProjectDialog::validateInput);
}

QString NewProjectDialog::projectName() const
{
    return _nameEdit->text();
}

QString NewProjectDialog::projectDescription() const
{
    return _descriptionEdit->toPlainText();
}

QString NewProjectDialog::thumbnailPath() const
{
    return _thumbnailPath;
}

void NewProjectDialog::onBrowseThumbnail()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        "Select Thumbnail", "",
        "Images (*.png *.jpg *.jpeg *.bmp *.gif);;All Files (*)");
    
    if (!fileName.isEmpty()) {
        _thumbnailPath = fileName;
        QPixmap pixmap(fileName);
        if (!pixmap.isNull()) {
            _thumbnailPreview->setPixmap(pixmap.scaled(100, 100, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }
    }
}

void NewProjectDialog::validateInput()
{
    _okButton->setEnabled(!_nameEdit->text().isEmpty());
}