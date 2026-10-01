/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <qt-wrappers.hpp>

#include <QDir>
#include <QScopedValueRollback>

#include <cmath>

namespace {
struct SourceCanvasDimensions {
	obs_sceneitem_t *item = nullptr;
	obs_source_t *source = nullptr;
	uint32_t nativeWidth = 0;
	uint32_t nativeHeight = 0;
	uint32_t visibleWidth = 0;
	uint32_t visibleHeight = 0;
	obs_sceneitem_crop crop = {};
};

static bool GetSourceCanvasDimensions(obs_sceneitem_t *item, SourceCanvasDimensions &dimensions)
{
	if (!item || obs_sceneitem_is_group(item)) {
		return false;
	}

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source || !(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO)) {
		return false;
	}

	dimensions.item = item;
	dimensions.source = source;
	dimensions.nativeWidth = obs_source_get_width(source);
	dimensions.nativeHeight = obs_source_get_height(source);
	obs_sceneitem_get_crop(item, &dimensions.crop);

	const int64_t visibleWidth = int64_t(dimensions.nativeWidth) - dimensions.crop.left - dimensions.crop.right;
	const int64_t visibleHeight = int64_t(dimensions.nativeHeight) - dimensions.crop.top - dimensions.crop.bottom;
	if (visibleWidth < 32 || visibleHeight < 32) {
		return false;
	}

	dimensions.visibleWidth = uint32_t(visibleWidth);
	dimensions.visibleHeight = uint32_t(visibleHeight);
	return true;
}

static void SetSourceTransformOneToOne(obs_sceneitem_t *item)
{
	obs_transform_info info = {};
	vec2_set(&info.pos, 0.0f, 0.0f);
	vec2_set(&info.scale, 1.0f, 1.0f);
	info.rot = 0.0f;
	info.alignment = OBS_ALIGN_TOP | OBS_ALIGN_LEFT;
	info.bounds_type = OBS_BOUNDS_NONE;
	info.bounds_alignment = OBS_ALIGN_CENTER;
	info.crop_to_bounds = false;
	vec2_set(&info.bounds, 0.0f, 0.0f);

	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_info2(item, &info);
	obs_sceneitem_defer_update_end(item);
}
} // namespace

void OBSBasic::ResetOutputs()
{
	ProfileScope("OBSBasic::ResetOutputs");

	const char *mode = config_get_string(activeConfiguration, "Output", "Mode");
	bool advOut = astrcmpi(mode, "Advanced") == 0;

	if ((!outputHandler || !outputHandler->Active()) &&
	    (!setupStreamingGuard.valid() ||
	     setupStreamingGuard.wait_for(std::chrono::seconds{0}) == std::future_status::ready)) {
		outputHandler.reset();
		outputHandler.reset(advOut ? CreateAdvancedOutputHandler(this) : CreateSimpleOutputHandler(this));

		emit ReplayBufEnabled(outputHandler->replayBuffer);

		if (sysTrayReplayBuffer) {
			sysTrayReplayBuffer->setEnabled(!!outputHandler->replayBuffer);
		}

		UpdateIsRecordingPausable();
	} else {
		outputHandler->Update();
	}
}

bool OBSBasic::Active() const
{
	if (!outputHandler) {
		return false;
	}
	return outputHandler->Active();
}

void OBSBasic::ResizeOutputSizeOfSource()
{
	ResizeCanvasToSource();
	UpdateSourceCanvasResolution();
}

OBSSceneItem OBSBasic::GetCanvasFitSource()
{
	const auto selectionModel = ui->sources->selectionModel();
	if (selectionModel && selectionModel->selectedIndexes().count() == 1) {
		OBSSceneItem selected = GetCurrentSceneItem();
		SourceCanvasDimensions dimensions;
		if (GetSourceCanvasDimensions(selected, dimensions)) {
			return selected;
		}
	}

	return {};
}

bool OBSBasic::CanResizeCanvasToSource()
{
	if (!loaded || isClosing_ || obs_video_active() || sourceCanvasResizeInProgress) {
		return false;
	}

	return bool(GetCanvasFitSource());
}

bool OBSBasic::ResizeCanvasToSource()
{
	if (!CanResizeCanvasToSource()) {
		return false;
	}

	// Retain the scene and selected item while the confirmation dialog runs.
	// Revalidate selection, dimensions and output state before changing video.
	OBSScene scene = GetCurrentScene();
	OBSSceneItem item = GetCanvasFitSource();
	QScopedValueRollback<bool> resizeGuard(sourceCanvasResizeInProgress, true);
	UpdateSourceCanvasResolution();
	SourceCanvasDimensions dimensions;
	if (isClosing_ || obs_video_active() || GetCurrentScene() != scene ||
	    GetCanvasFitSource() != item || !GetSourceCanvasDimensions(item, dimensions)) {
		return false;
	}

	QMessageBox resize_output(this);
	resize_output.setTextFormat(Qt::PlainText);
	resize_output.setText(QTStr("ResizeOutputSizeOfSource.Text")
				      .arg(QT_UTF8(obs_source_get_name(dimensions.source)))
				      .arg(dimensions.visibleWidth)
				      .arg(dimensions.visibleHeight) +
			      "\n\n" + QTStr("ResizeOutputSizeOfSource.Continue"));
	QAbstractButton *Yes = resize_output.addButton(QTStr("Yes"), QMessageBox::YesRole);
	resize_output.addButton(QTStr("No"), QMessageBox::NoRole);
	resize_output.setIcon(QMessageBox::Warning);
	resize_output.setWindowTitle(QTStr("ResizeOutputSizeOfSource"));
	resize_output.exec();

	if (resize_output.clickedButton() != Yes) {
		return false;
	}

	SourceCanvasDimensions currentDimensions;
	if (isClosing_ || obs_video_active() || GetCurrentScene() != scene ||
	    GetCanvasFitSource() != item || !GetSourceCanvasDimensions(item, currentDimensions) ||
	    currentDimensions.visibleWidth != dimensions.visibleWidth ||
	    currentDimensions.visibleHeight != dimensions.visibleHeight) {
		blog(LOG_WARNING, "Canvas fit cancelled: source or output changed while confirming");
		QMessageBox::information(this, QTStr("ResizeOutputSizeOfSource"),
					 QTStr("ResizeOutputSizeOfSource.Changed"));
		return false;
	}

	const uint32_t oldBaseWidth = config_get_uint(activeConfiguration, "Video", "BaseCX");
	const uint32_t oldBaseHeight = config_get_uint(activeConfiguration, "Video", "BaseCY");
	const uint32_t oldOutputWidth = config_get_uint(activeConfiguration, "Video", "OutputCX");
	const uint32_t oldOutputHeight = config_get_uint(activeConfiguration, "Video", "OutputCY");
	const bool resolutionChanged =
		oldBaseWidth != dimensions.visibleWidth || oldBaseHeight != dimensions.visibleHeight ||
		oldOutputWidth != dimensions.visibleWidth || oldOutputHeight != dimensions.visibleHeight;

	if (resolutionChanged) {
		config_set_uint(activeConfiguration, "Video", "BaseCX", dimensions.visibleWidth);
		config_set_uint(activeConfiguration, "Video", "BaseCY", dimensions.visibleHeight);
		config_set_uint(activeConfiguration, "Video", "OutputCX", dimensions.visibleWidth);
		config_set_uint(activeConfiguration, "Video", "OutputCY", dimensions.visibleHeight);

		const int resetResult = ResetVideo();
		if (resetResult != OBS_VIDEO_SUCCESS) {
			config_set_uint(activeConfiguration, "Video", "BaseCX", oldBaseWidth);
			config_set_uint(activeConfiguration, "Video", "BaseCY", oldBaseHeight);
			config_set_uint(activeConfiguration, "Video", "OutputCX", oldOutputWidth);
			config_set_uint(activeConfiguration, "Video", "OutputCY", oldOutputHeight);
			ResetVideo();
			blog(LOG_ERROR, "Failed to fit the canvas to source '%s' (%ux%u), error %d",
			     obs_source_get_name(dimensions.source), dimensions.visibleWidth, dimensions.visibleHeight,
			     resetResult);
			return false;
		}
		ResetOutputs();
	}

	SetSourceTransformOneToOne(dimensions.item);
	activeConfiguration.SaveSafe("tmp");
	SaveProject();

	blog(LOG_INFO, "Canvas and output explicitly fitted to source '%s' at %ux%u (native %ux%u)",
	     obs_source_get_name(dimensions.source), dimensions.visibleWidth, dimensions.visibleHeight,
	     dimensions.nativeWidth, dimensions.nativeHeight);
	return true;
}

void OBSBasic::UpdateSourceCanvasResolution()
{
	if (!loaded || isClosing_) {
		return;
	}

	// This observer is read-only: selecting a source or waiting for PipeWire
	// must never change the canvas or the composition of a multi-source scene.
	ui->fitCanvasToSourceButton->setEnabled(CanResizeCanvasToSource());
	ui->fitCanvasToSourceButton->setToolTip(QTStr(obs_video_active() ? "ResizeOutputSizeOfSource.Active"
						      : ui->fitCanvasToSourceButton->isEnabled()
							      ? "ResizeOutputSizeOfSource.ToolTip"
							      : "ResizeOutputSizeOfSource.NoSource"));

	obs_video_info videoInfo = {};
	const bool haveVideoInfo = obs_get_video_info(&videoInfo);
	const uint32_t canvasWidth = haveVideoInfo ? videoInfo.base_width
						   : config_get_uint(activeConfiguration, "Video", "BaseCX");
	const uint32_t canvasHeight = haveVideoInfo ? videoInfo.base_height
						    : config_get_uint(activeConfiguration, "Video", "BaseCY");
	const uint32_t outputWidth = haveVideoInfo ? videoInfo.output_width
						   : config_get_uint(activeConfiguration, "Video", "OutputCX");
	const uint32_t outputHeight = haveVideoInfo ? videoInfo.output_height
						    : config_get_uint(activeConfiguration, "Video", "OutputCY");

	QItemSelectionModel *selectionModel = ui->sources->selectionModel();
	if (!selectionModel) {
		ui->sourceCanvasResolutionLabel->setText(QTStr("Basic.SourceCanvasResolution.Initializing"));
		return;
	}

	if (sourceCanvasSelectionModel != selectionModel) {
		sourceCanvasSelectionModel = selectionModel;
		connect(selectionModel, &QItemSelectionModel::selectionChanged, this,
			&OBSBasic::UpdateSourceCanvasResolution);
	}

	const QModelIndexList selectedItems = selectionModel->selectedIndexes();
	OBSSceneItem fitSource = GetCanvasFitSource();
	if (!fitSource && selectedItems.count() != 1) {
		ui->sourceCanvasResolutionLabel->setText(QTStr(selectedItems.empty()
								       ? "Basic.SourceCanvasResolution.None"
								       : "Basic.SourceCanvasResolution.Multiple")
								 .arg(canvasWidth)
								 .arg(canvasHeight)
								 .arg(outputWidth)
								 .arg(outputHeight));
		ui->sourceCanvasResolutionLabel->setToolTip(ui->sourceCanvasResolutionLabel->text());
		return;
	}

	SourceCanvasDimensions dimensions;
	if (!GetSourceCanvasDimensions(fitSource, dimensions)) {
		ui->sourceCanvasResolutionLabel->setText(QTStr("Basic.SourceCanvasResolution.Waiting")
								 .arg(canvasWidth)
								 .arg(canvasHeight)
								 .arg(outputWidth)
								 .arg(outputHeight));
		ui->sourceCanvasResolutionLabel->setToolTip(ui->sourceCanvasResolutionLabel->text());
		return;
	}

	obs_transform_info transform = {};
	obs_sceneitem_get_info2(dimensions.item, &transform);
	const bool oneToOne = std::abs(transform.pos.x) < 0.0001f && std::abs(transform.pos.y) < 0.0001f &&
			      transform.bounds_type == OBS_BOUNDS_NONE &&
			      std::abs(transform.scale.x - 1.0f) < 0.0001f &&
			      std::abs(transform.scale.y - 1.0f) < 0.0001f && std::abs(transform.rot) < 0.0001f &&
			      transform.alignment == (OBS_ALIGN_TOP | OBS_ALIGN_LEFT) && !transform.crop_to_bounds;
	const bool canvasMatches = canvasWidth == dimensions.visibleWidth && canvasHeight == dimensions.visibleHeight;
	const uint32_t alignedOutputWidth = dimensions.visibleWidth & ~uint32_t(3);
	const uint32_t alignedOutputHeight = dimensions.visibleHeight & ~uint32_t(1);
	const bool outputMatches = outputWidth == alignedOutputWidth && outputHeight == alignedOutputHeight;
	const bool cropped = dimensions.crop.left || dimensions.crop.right || dimensions.crop.top ||
			     dimensions.crop.bottom;
	QString sourceLine;
	if (cropped) {
		sourceLine = QTStr("Basic.SourceCanvasResolution.SourceCropped")
				     .arg(dimensions.visibleWidth)
				     .arg(dimensions.visibleHeight)
				     .arg(dimensions.nativeWidth)
				     .arg(dimensions.nativeHeight);
	} else {
		sourceLine = QTStr("Basic.SourceCanvasResolution.Source")
				     .arg(dimensions.visibleWidth)
				     .arg(dimensions.visibleHeight);
	}

	QString status;
	if (canvasMatches && outputMatches && oneToOne) {
		status.clear();
	} else if (obs_video_active()) {
		status = QTStr("Basic.SourceCanvasResolution.Active");
	} else {
		status = QTStr("Basic.SourceCanvasResolution.Mismatch");
	}

	QString details = QTStr("Basic.SourceCanvasResolution.Details")
				  .arg(sourceLine)
				  .arg(canvasWidth)
				  .arg(canvasHeight)
				  .arg(outputWidth)
				  .arg(outputHeight);
	if (!status.isEmpty()) {
		details += QStringLiteral(" — ") + status;
	}
	ui->sourceCanvasResolutionLabel->setText(details);
	ui->sourceCanvasResolutionLabel->setToolTip(details);
}

const char *OBSBasic::GetCurrentOutputPath()
{
	const char *path = nullptr;
	const char *mode = config_get_string(Config(), "Output", "Mode");

	if (strcmp(mode, "Advanced") == 0) {
		const char *advanced_mode = config_get_string(Config(), "AdvOut", "RecType");

		if (strcmp(advanced_mode, "FFmpeg") == 0) {
			path = config_get_string(Config(), "AdvOut", "FFFilePath");
		} else {
			path = config_get_string(Config(), "AdvOut", "RecFilePath");
		}
	} else {
		path = config_get_string(Config(), "SimpleOutput", "FilePath");
	}

	return path;
}

void OBSBasic::OutputPathInvalidMessage()
{
	blog(LOG_ERROR, "Recording stopped because of bad output path");

	OBSMessageBox::critical(this, QTStr("Output.BadPath.Title"), QTStr("Output.BadPath.Text"));
}

bool OBSBasic::IsFFmpegOutputToURL() const
{
	const char *mode = config_get_string(Config(), "Output", "Mode");
	if (strcmp(mode, "Advanced") == 0) {
		const char *advanced_mode = config_get_string(Config(), "AdvOut", "RecType");
		if (strcmp(advanced_mode, "FFmpeg") == 0) {
			bool is_local = config_get_bool(Config(), "AdvOut", "FFOutputToFile");
			if (!is_local) {
				return true;
			}
		}
	}

	return false;
}

bool OBSBasic::OutputPathValid()
{
	if (IsFFmpegOutputToURL()) {
		return true;
	}

	const char *path = GetCurrentOutputPath();
	return path && *path && QDir(path).exists();
}
